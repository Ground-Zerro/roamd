#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>

#include "roamd.h"
#include "mesh.h"

#define NL_BUF	1024

struct nl_req {
	struct nlmsghdr n;
	struct ifinfomsg i;
	char buf[512];
};

static int nl_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);

	if (fd < 0)
		return -1;

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		close(fd);
		return -1;
	}

	return fd;
}

static struct rtattr *nl_attr(struct nl_req *r, int type, const void *data, int len)
{
	struct rtattr *rta = (struct rtattr *)((char *)r + NLMSG_ALIGN(r->n.nlmsg_len));

	if (NLMSG_ALIGN(r->n.nlmsg_len) + RTA_LENGTH(len) > sizeof(*r))
		return NULL;

	rta->rta_type = type;
	rta->rta_len = RTA_LENGTH(len);
	if (len)
		memcpy(RTA_DATA(rta), data, len);

	r->n.nlmsg_len = NLMSG_ALIGN(r->n.nlmsg_len) + RTA_ALIGN(rta->rta_len);

	return rta;
}

static void nl_nest_end(struct nl_req *r, struct rtattr *nest)
{
	nest->rta_len = (char *)r + NLMSG_ALIGN(r->n.nlmsg_len) - (char *)nest;
}

static bool nl_talk(struct nl_req *r)
{
	char buf[NL_BUF];
	struct nlmsghdr *h;
	ssize_t len;
	int fd = nl_open();
	bool ok = false;

	if (fd < 0)
		return false;

	r->n.nlmsg_seq = 1;
	r->n.nlmsg_flags |= NLM_F_ACK | NLM_F_REQUEST;

	if (send(fd, r, r->n.nlmsg_len, 0) < 0) {
		close(fd);
		return false;
	}

	len = recv(fd, buf, sizeof(buf), 0);
	for (h = (struct nlmsghdr *)buf; len > 0 && NLMSG_OK(h, (size_t)len);
	     h = NLMSG_NEXT(h, len)) {
		if (h->nlmsg_type != NLMSG_ERROR)
			continue;

		ok = ((struct nlmsgerr *)NLMSG_DATA(h))->error == 0;
		break;
	}

	close(fd);

	return ok;
}

bool mesh_link_exists(const char *name)
{
	return if_nametoindex(name) != 0;
}

static bool mesh_vlan_add(const char *parent, int vid, const char *name)
{
	struct nl_req r = { 0 };
	struct rtattr *info, *data;
	unsigned int idx = if_nametoindex(parent);
	uint16_t id = vid;

	if (!idx || mesh_link_exists(name))
		return mesh_link_exists(name);

	r.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	r.n.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;
	r.n.nlmsg_type = RTM_NEWLINK;
	r.i.ifi_family = AF_UNSPEC;

	nl_attr(&r, IFLA_LINK, &idx, sizeof(idx));
	nl_attr(&r, IFLA_IFNAME, name, strlen(name) + 1);

	info = nl_attr(&r, IFLA_LINKINFO, NULL, 0);
	if (!info)
		return false;

	nl_attr(&r, IFLA_INFO_KIND, "vlan", 4);
	data = nl_attr(&r, IFLA_INFO_DATA, NULL, 0);
	if (!data)
		return false;

	nl_attr(&r, IFLA_VLAN_ID, &id, sizeof(id));
	nl_nest_end(&r, data);
	nl_nest_end(&r, info);

	return nl_talk(&r);
}

bool mesh_link_del(const char *name)
{
	struct nl_req r = { 0 };
	unsigned int idx = if_nametoindex(name);

	if (!idx)
		return true;

	r.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	r.n.nlmsg_type = RTM_DELLINK;
	r.i.ifi_family = AF_UNSPEC;
	r.i.ifi_index = idx;

	return nl_talk(&r);
}

static bool mesh_link_master(const char *name, const char *master)
{
	struct nl_req r = { 0 };
	unsigned int idx = if_nametoindex(name);
	unsigned int midx = master ? if_nametoindex(master) : 0;

	if (!idx || (master && !midx))
		return false;

	r.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	r.n.nlmsg_type = RTM_NEWLINK;
	r.i.ifi_family = AF_UNSPEC;
	r.i.ifi_index = idx;

	nl_attr(&r, IFLA_MASTER, &midx, sizeof(midx));

	return nl_talk(&r);
}

static bool mesh_link_up(const char *name)
{
	struct nl_req r = { 0 };
	unsigned int idx = if_nametoindex(name);

	if (!idx)
		return false;

	r.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	r.n.nlmsg_type = RTM_NEWLINK;
	r.i.ifi_family = AF_UNSPEC;
	r.i.ifi_index = idx;
	r.i.ifi_flags = IFF_UP;
	r.i.ifi_change = IFF_UP;

	return nl_talk(&r);
}

bool mesh_link_enslave(const char *parent, int vid, const char *name, const char *bridge)
{
	if (!mesh_vlan_add(parent, vid, name))
		return false;

	if (!mesh_link_up(name))
		return false;

	return mesh_link_master(name, bridge);
}
