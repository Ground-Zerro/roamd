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

#include "roamd.h"
#include "mesh.h"

#define NEIGH_BUF	16384

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

			memset(&out[n], 0, sizeof(out[n]));
			snprintf(out[n].addr, sizeof(out[n].addr), "%s", addr);
			snprintf(out[n].mac, sizeof(out[n].mac), "%s", mac);
			out[n].v6 = nd->ndm_family == AF_INET6;
			out[n].ll = out[n].v6 && !strncasecmp(addr, "fe80:", 5);
			n++;
		}

		if (done)
			break;
	}

	free(buf);
	close(fd);

	return n;
}

static void neigh_warm6(const char *ifname)
{
	struct sockaddr_in6 to = { .sin6_family = AF_INET6 };
	struct icmp6_probe {
		uint8_t type;
		uint8_t code;
		uint16_t checksum;
		uint16_t id;
		uint16_t seq;
	} probe = { .type = 128, .id = htons(0x524d) };
	unsigned int idx = ifname ? if_nametoindex(ifname) : 0;
	int fd;

	if (!idx)
		return;

	fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMPV6);
	if (fd < 0)
		return;

	setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));

	if (inet_pton(AF_INET6, "ff02::1", &to.sin6_addr) == 1) {
		to.sin6_scope_id = idx;
		sendto(fd, &probe, sizeof(probe), MSG_DONTWAIT,
		       (struct sockaddr *)&to, sizeof(to));
	}

	close(fd);
}

void mesh_neigh_warm(const char *ifname, const char *base)
{
	struct sockaddr_in to = { .sin_family = AF_INET };
	struct icmp_probe {
		uint8_t type;
		uint8_t code;
		uint16_t checksum;
		uint16_t id;
		uint16_t seq;
	} probe = { .type = 8, .id = htons(0x524d) };
	unsigned int host;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);
	if (fd < 0)
		fd = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP);

	if (fd < 0)
		return;

	if (ifname)
		setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));

	for (host = 1; host < 255; host++) {
		char addr[INET_ADDRSTRLEN];
		uint32_t sum = 0;
		const uint8_t *p = (const uint8_t *)&probe;
		size_t i;

		snprintf(addr, sizeof(addr), "%s.%u", base, host);

		if (!inet_pton(AF_INET, addr, &to.sin_addr))
			continue;

		probe.seq = htons(host);
		probe.checksum = 0;

		for (i = 0; i < sizeof(probe); i += 2)
			sum += (uint32_t)p[i] << 8 | p[i + 1];

		while (sum >> 16)
			sum = (sum & 0xffff) + (sum >> 16);

		probe.checksum = htons((uint16_t)~sum);

		if (sendto(fd, &probe, sizeof(probe), MSG_DONTWAIT,
			   (struct sockaddr *)&to, sizeof(to)) < 0)
			continue;
	}

	close(fd);
	neigh_warm6(ifname);
}
