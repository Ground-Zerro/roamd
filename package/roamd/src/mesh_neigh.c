#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <poll.h>

#include "roamd.h"
#include "mesh.h"

#define NEIGH_BUF	16384
#define WARM6_ROUNDS	2
#define WARM6_LISTEN	500
#define WARM6_PEERS	64

struct neigh_req {
	struct nlmsghdr n;
	struct ndmsg nd;
};

unsigned int mesh_neigh_dump(const char *ifname, struct mesh_neigh *out, unsigned int max)
{
	struct neigh_req req = { 0 };
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	unsigned int want = ifname ? if_nametoindex(ifname) : 0;
	unsigned int n = 0;
	char *buf;
	int fd;

	if (ifname && !want)
		return 0;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return 0;

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		close(fd);

		return 0;
	}

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
	req.n.nlmsg_type = RTM_GETNEIGH;
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.n.nlmsg_seq = 1;
	req.nd.ndm_family = AF_UNSPEC;

	if (send(fd, &req, req.n.nlmsg_len, 0) < 0) {
		close(fd);

		return 0;
	}

	buf = malloc(NEIGH_BUF);
	if (!buf) {
		close(fd);

		return 0;
	}

	for (;;) {
		struct nlmsghdr *h;
		ssize_t len = recv(fd, buf, NEIGH_BUF, 0);
		bool done = false;

		if (len <= 0)
			break;

		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)len);
		     h = NLMSG_NEXT(h, len)) {
			struct ndmsg *nd = NLMSG_DATA(h);
			struct rtattr *rta;
			char addr[INET6_ADDRSTRLEN] = "";
			char mac[MESH_MAC_MAX] = "";
			size_t rlen;

			if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) {
				done = true;
				break;
			}

			if (h->nlmsg_type != RTM_NEWNEIGH || n >= max)
				continue;

			if (want && (unsigned int)nd->ndm_ifindex != want)
				continue;

			if (nd->ndm_state & (NUD_FAILED | NUD_INCOMPLETE | NUD_NOARP))
				continue;

			rlen = h->nlmsg_len - NLMSG_LENGTH(sizeof(*nd));

			for (rta = (struct rtattr *)((char *)nd + NLMSG_ALIGN(sizeof(*nd)));
			     RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen)) {
				if (rta->rta_type == NDA_DST)
					inet_ntop(nd->ndm_family, RTA_DATA(rta), addr,
						  sizeof(addr));
				else if (rta->rta_type == NDA_LLADDR &&
					 RTA_PAYLOAD(rta) == 6)
					roam_mac_str(RTA_DATA(rta), mac, sizeof(mac));
			}

			if (!addr[0] || !mac[0])
				continue;

			if (nd->ndm_family == AF_INET6 && strncasecmp(addr, "fe80:", 5))
				continue;

			memset(&out[n], 0, sizeof(out[n]));
			snprintf(out[n].addr, sizeof(out[n].addr), "%s", addr);
			snprintf(out[n].mac, sizeof(out[n].mac), "%s", mac);
			out[n].v6 = nd->ndm_family == AF_INET6;
			n++;
		}

		if (done)
			break;
	}

	free(buf);
	close(fd);

	return n;
}

int mesh_icmp_socket(int family)
{
	int proto = family == AF_INET6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;
	int fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, proto);

	return fd < 0 ? socket(family, SOCK_RAW | SOCK_CLOEXEC, proto) : fd;
}

uint16_t mesh_icmp_sum(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t sum = 0;

	for (; len > 1; p += 2, len -= 2)
		sum += (uint32_t)p[0] << 8 | p[1];

	if (len)
		sum += (uint32_t)p[0] << 8;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return htons((uint16_t)~sum);
}

void mesh_neigh_warm4(const char *ifname, const char *base)
{
	struct sockaddr_in to = { .sin_family = AF_INET };
	struct icmphdr probe = { .type = ICMP_ECHO, .un.echo.id = htons(MESH_PROBE_ID) };
	unsigned int host;
	int fd = mesh_icmp_socket(AF_INET);

	if (fd < 0)
		return;

	setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));

	for (host = 1; host < 255; host++) {
		char addr[INET_ADDRSTRLEN];

		snprintf(addr, sizeof(addr), "%s.%u", base, host);

		if (inet_pton(AF_INET, addr, &to.sin_addr) != 1)
			continue;

		probe.un.echo.sequence = htons(host);
		probe.checksum = 0;
		probe.checksum = mesh_icmp_sum(&probe, sizeof(probe));

		sendto(fd, &probe, sizeof(probe), MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to));
	}

	close(fd);
}

static void echo6_send(int fd, const struct sockaddr_in6 *to, uint16_t seq)
{
	struct icmp6_hdr probe = { .icmp6_type = ICMP6_ECHO_REQUEST };

	probe.icmp6_id = htons(MESH_PROBE_ID);
	probe.icmp6_seq = htons(seq);

	sendto(fd, &probe, sizeof(probe), MSG_DONTWAIT, (const struct sockaddr *)to, sizeof(*to));
}

static unsigned int echo6_collect(int fd, struct in6_addr *peer, unsigned int n, unsigned int max)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	unsigned int left = WARM6_PEERS * WARM6_ROUNDS * 2;

	while (left-- && poll(&pfd, 1, WARM6_LISTEN) > 0) {
		struct sockaddr_in6 from;
		socklen_t len = sizeof(from);
		uint8_t type;
		unsigned int i;

		if (recvfrom(fd, &type, sizeof(type), MSG_DONTWAIT | MSG_TRUNC,
			     (struct sockaddr *)&from, &len) <= 0)
			continue;

		if (type != ICMP6_ECHO_REPLY || !IN6_IS_ADDR_LINKLOCAL(&from.sin6_addr))
			continue;

		for (i = 0; i < n && memcmp(&peer[i], &from.sin6_addr, sizeof(peer[i])); i++)
			;

		if (i == n && n < max)
			peer[n++] = from.sin6_addr;
	}

	return n;
}

void mesh_neigh_warm6(const char *ifname)
{
	struct sockaddr_in6 to = { .sin6_family = AF_INET6 };
	struct in6_addr peer[WARM6_PEERS];
	struct icmp6_filter filter;
	unsigned int idx = if_nametoindex(ifname), n = 0, i;
	uint16_t seq;
	int fd;

	if (!idx)
		return;

	fd = mesh_icmp_socket(AF_INET6);
	if (fd < 0)
		return;

	ICMP6_FILTER_SETBLOCKALL(&filter);
	ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
	setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter));
	setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));

	inet_pton(AF_INET6, "ff02::1", &to.sin6_addr);
	to.sin6_scope_id = idx;

	for (seq = 0; seq < WARM6_ROUNDS; seq++) {
		echo6_send(fd, &to, seq);
		n = echo6_collect(fd, peer, n, WARM6_PEERS);
	}

	for (i = 0; i < n; i++) {
		to.sin6_addr = peer[i];
		echo6_send(fd, &to, seq + i);
	}

	close(fd);
}
