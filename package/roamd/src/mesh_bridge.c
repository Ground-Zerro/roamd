#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <net/if.h>
#include <netinet/ether.h>

#include "roamd.h"
#include "mesh.h"

#define BR_SYS		"/sys/class/net"
#define FDB_ENTRY_MAX	512

struct fdb_entry {
	uint8_t mac[6];
	uint8_t port_no;
	uint8_t is_local;
	uint32_t ageing;
	uint8_t port_hi;
	uint8_t pad;
	uint16_t unused;
};

struct br_port {
	char ifname[IFNAMSIZ];
	uint16_t no;
	bool wireless;
};

struct br_view {
	char name[IFNAMSIZ];
	struct br_port ports[MESH_BRPORT_MAX];
	unsigned int n_ports;
	struct fdb_entry fdb[FDB_ENTRY_MAX];
	unsigned int n_fdb;
};

static bool sys_flag(const char *ifname, const char *node)
{
	char path[320];

	snprintf(path, sizeof(path), BR_SYS "/%s/%s", ifname, node);

	return !access(path, F_OK);
}

static long sys_num(const char *ifname, const char *node)
{
	char path[320], buf[32];
	FILE *f;
	long v = -1;

	snprintf(path, sizeof(path), BR_SYS "/%s/%s", ifname, node);
	f = fopen(path, "r");
	if (!f)
		return -1;

	if (fgets(buf, sizeof(buf), f))
		v = strtol(buf, NULL, 0);

	fclose(f);

	return v;
}

static bool br_ports_load(struct br_view *v)
{
	char path[320];
	struct dirent *de;
	DIR *d;

	snprintf(path, sizeof(path), BR_SYS "/%s/brif", v->name);
	d = opendir(path);
	if (!d)
		return false;

	while ((de = readdir(d)) && v->n_ports < MESH_BRPORT_MAX) {
		struct br_port *p = &v->ports[v->n_ports];
		long no;

		if (de->d_name[0] == '.')
			continue;

		no = sys_num(de->d_name, "brport/port_no");
		if (no < 0)
			continue;

		strncpy(p->ifname, de->d_name, sizeof(p->ifname) - 1);
		p->ifname[sizeof(p->ifname) - 1] = '\0';
		p->no = no;
		p->wireless = sys_flag(de->d_name, "phy80211");
		v->n_ports++;
	}

	closedir(d);

	return v->n_ports > 0;
}

static void br_fdb_load(struct br_view *v)
{
	char path[320];
	FILE *f;

	snprintf(path, sizeof(path), BR_SYS "/%s/brforward", v->name);
	f = fopen(path, "r");
	if (!f)
		return;

	v->n_fdb = fread(v->fdb, sizeof(v->fdb[0]), FDB_ENTRY_MAX, f);
	fclose(f);
}

static const struct br_port *port_by_no(const struct br_view *v, uint16_t no)
{
	unsigned int i;

	for (i = 0; i < v->n_ports; i++)
		if (v->ports[i].no == no)
			return &v->ports[i];

	return NULL;
}

static uint16_t entry_port(const struct fdb_entry *e)
{
	return e->port_no | ((uint16_t)e->port_hi << 8);
}

static bool br_next(DIR *d, struct br_view *v)
{
	struct dirent *de;

	while ((de = readdir(d))) {
		char path[320];

		if (de->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), BR_SYS "/%s/bridge", de->d_name);
		if (access(path, F_OK))
			continue;

		memset(v, 0, sizeof(*v));
		strncpy(v->name, de->d_name, sizeof(v->name) - 1);
		v->name[sizeof(v->name) - 1] = '\0';

		if (!br_ports_load(v))
			continue;

		br_fdb_load(v);

		return true;
	}

	return false;
}

bool mesh_bridge_port_of(const uint8_t *mac, struct mesh_uplink *out)
{
	struct br_view v;
	DIR *d = opendir(BR_SYS);
	bool found = false;

	if (!d)
		return false;

	while (!found && br_next(d, &v)) {
		unsigned int i;

		for (i = 0; i < v.n_fdb; i++) {
			const struct br_port *p;

			if (v.fdb[i].is_local || memcmp(v.fdb[i].mac, mac, 6))
				continue;

			p = port_by_no(&v, entry_port(&v.fdb[i]));
			if (!p)
				continue;

			strncpy(out->ifname, p->ifname, sizeof(out->ifname) - 1);
			out->ifname[sizeof(out->ifname) - 1] = '\0';
			out->wireless = p->wireless;
			found = true;
			break;
		}
	}

	closedir(d);

	return found;
}

static const char *member_by_mac(const uint8_t *mac)
{
	struct mesh_member *m;

	list_for_each_entry(m, &mesh_members, list) {
		struct ether_addr ea;

		if (ether_aton_r(m->mac, &ea) && !memcmp(ea.ether_addr_octet, mac, 6))
			return m->id;
	}

	return NULL;
}

static bool mac_is_member(const uint8_t *mac)
{
	return member_by_mac(mac) != NULL;
}

static const char *member_on_port(const struct br_view *v, const struct br_port *p)
{
	unsigned int i;

	for (i = 0; i < v->n_fdb; i++) {
		const char *id;

		if (v->fdb[i].is_local || entry_port(&v->fdb[i]) != p->no)
			continue;

		id = member_by_mac(v->fdb[i].mac);
		if (id)
			return id;
	}

	return NULL;
}

static const struct br_port *port_of_mac(const struct br_view *v, const uint8_t *mac)
{
	unsigned int i;

	for (i = 0; i < v->n_fdb; i++) {
		if (v->fdb[i].is_local || memcmp(v->fdb[i].mac, mac, 6))
			continue;

		return port_by_no(v, entry_port(&v->fdb[i]));
	}

	return NULL;
}

const char *mesh_bridge_member_behind(const uint8_t *mac)
{
	struct br_view v;
	DIR *d = opendir(BR_SYS);
	const char *owner = NULL;

	if (!d)
		return NULL;

	while (!owner && br_next(d, &v)) {
		const struct br_port *p = port_of_mac(&v, mac);

		if (p)
			owner = member_on_port(&v, p);
	}

	closedir(d);

	return owner;
}

void mesh_wired_collect(struct mesh_assoc_idx *idx, const uint8_t *parent)
{
	struct br_view v;
	DIR *d = opendir(BR_SYS);

	if (!d)
		return;

	while (br_next(d, &v)) {
		const struct br_port *uplink = NULL;
		unsigned int i;

		if (parent) {
			uplink = port_of_mac(&v, parent);
			if (!uplink)
				continue;
		}

		for (i = 0; i < v.n_fdb && idx->n < MESH_ASSOC_MAX; i++) {
			const struct br_port *p;
			struct mesh_assoc *a;
			unsigned int k;
			bool dup = false;

			if (v.fdb[i].is_local)
				continue;

			p = port_by_no(&v, entry_port(&v.fdb[i]));
			if (!p || p->wireless || p == uplink)
				continue;

			if (mac_is_member(v.fdb[i].mac))
				continue;

			if (member_on_port(&v, p))
				continue;

			for (k = 0; k < idx->n; k++)
				if (!memcmp(idx->e[k].addr, v.fdb[i].mac, 6)) {
					dup = true;
					break;
				}

			if (dup)
				continue;

			a = &idx->e[idx->n++];
			memset(a, 0, sizeof(*a));
			memcpy(a->addr, v.fdb[i].mac, 6);
			a->wired = true;
		}
	}

	closedir(d);
}
