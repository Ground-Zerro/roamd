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
#define STP_WIFI_COST	1000
#define STP_ROOT_PRIORITY	4096

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

static bool sys_line(const char *ifname, const char *node, char *buf, size_t len)
{
	char path[320];
	FILE *f;
	bool got;

	snprintf(path, sizeof(path), BR_SYS "/%s/%s", ifname, node);
	f = fopen(path, "r");
	if (!f)
		return false;

	got = fgets(buf, len, f) != NULL;
	fclose(f);

	if (got)
		buf[strcspn(buf, "\r\n")] = '\0';

	return got;
}

static long sys_num(const char *ifname, const char *node)
{
	char buf[32];

	return sys_line(ifname, node, buf, sizeof(buf)) ? strtol(buf, NULL, 0) : -1;
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

bool mesh_bridge_lan(char *out, size_t len)
{
	struct uci_session u;
	struct uci_section *lan;
	const char *dev;
	bool ok = false;

	if (!uci_session_open(&u, "network"))
		return false;

	lan = uci_lookup_section(u.ctx, u.pkg, "lan");
	dev = lan ? uci_lookup_option_string(u.ctx, lan, "device") : NULL;
	if (dev) {
		snprintf(out, len, "%s", dev);
		ok = true;
	}

	uci_session_close(&u);

	return ok;
}

unsigned int mesh_bridge_ports(const char *bridge, char out[][IFNAMSIZ], unsigned int max)
{
	char path[320];
	struct dirent *de;
	unsigned int n = 0;
	DIR *d;

	snprintf(path, sizeof(path), BR_SYS "/%s/brif", bridge);
	d = opendir(path);
	if (!d)
		return 0;

	while ((de = readdir(d)) && n < max) {
		if (de->d_name[0] == '.')
			continue;

		snprintf(out[n], IFNAMSIZ, "%.*s", IFNAMSIZ - 1, de->d_name);
		n++;
	}

	closedir(d);

	return n;
}

bool mesh_bridge_wired_port(const char *bridge, char *out, size_t len)
{
	char ports[MESH_BRPORT_MAX][IFNAMSIZ];
	unsigned int n = mesh_bridge_ports(bridge, ports, MESH_BRPORT_MAX);
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (sys_flag(ports[i], "phy80211"))
			continue;

		snprintf(out, len, "%s", ports[i]);

		return true;
	}

	return false;
}

static void sys_write(const char *ifname, const char *node, const char *value)
{
	char path[320];
	FILE *f;

	snprintf(path, sizeof(path), BR_SYS "/%s/%s", ifname, node);
	f = fopen(path, "w");
	if (!f)
		return;

	fputs(value, f);
	fclose(f);
}

void mesh_bridge_stp_root(const char *bridge)
{
	if (sys_num(bridge, "bridge/stp_state") != 1)
		sys_write(bridge, "bridge/stp_state", "1");

	if (sys_num(bridge, "bridge/priority") != STP_ROOT_PRIORITY)
		sys_write(bridge, "bridge/priority", "4096");
}

void mesh_bridge_wifi_cost(void)
{
	DIR *d = opendir(BR_SYS);
	struct dirent *de;

	if (!d)
		return;

	while ((de = readdir(d))) {
		char path[320];
		FILE *f;

		if (de->d_name[0] == '.' || !sys_flag(de->d_name, "phy80211") ||
		    !sys_flag(de->d_name, "brport") ||
		    sys_num(de->d_name, "brport/path_cost") == STP_WIFI_COST)
			continue;

		snprintf(path, sizeof(path), BR_SYS "/%s/brport/path_cost", de->d_name);
		f = fopen(path, "w");
		if (!f)
			continue;

		fprintf(f, "%d", STP_WIFI_COST);
		fclose(f);
	}

	closedir(d);
}

bool mesh_bridge_root_is_self(const char *bridge)
{
	char root[32], self[32];

	return sys_line(bridge, "bridge/root_id", root, sizeof(root)) &&
	       sys_line(bridge, "bridge/bridge_id", self, sizeof(self)) &&
	       !strcmp(root, self);
}

bool mesh_bridge_carrier(const char *ifname)
{
	return sys_num(ifname, "carrier") == 1;
}

bool mesh_bridge_sta_name(char *out, size_t len)
{
	DIR *d = opendir(BR_SYS);
	struct dirent *de;
	char state[16];
	bool up = false;

	if (!d)
		return false;

	while (!up && (de = readdir(d))) {
		if (!strstr(de->d_name, "-sta") || !sys_flag(de->d_name, "phy80211") ||
		    !sys_line(de->d_name, "operstate", state, sizeof(state)) || strcmp(state, "up"))
			continue;

		if (out)
			snprintf(out, len, "%s", de->d_name);

		up = true;
	}

	closedir(d);

	return up;
}

bool mesh_bridge_sta_up(void)
{
	return mesh_bridge_sta_name(NULL, 0);
}
