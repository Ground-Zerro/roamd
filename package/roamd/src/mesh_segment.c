#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <net/if.h>
#include <netinet/ether.h>

#include "roamd.h"
#include "mesh.h"

#define SEG_SECTION	"roamd_seg"
#define SWITCH_LIST	"swconfig list 2>/dev/null"
#define SWITCH_SHOW	"swconfig dev %s show 2>/dev/null"
#define SWITCH_HELP	"swconfig dev %s help 2>&1"
#define SEG_NAME_MAX	24
#define SEG_LINK_MAX	(MESH_ALLOW_MAX + 2)
#define NET_SYS		"/sys/class/net"

struct seg_link {
	uint16_t vid;
	char bridge[IFNAMSIZ];
	bool usable;
};

static struct seg_link segs[MESH_NET_MAX];
static unsigned int segs_n;
static char seg_issue[96];
static const char *seg_trunk = "";

static void seg_bridge_name(char *out, size_t len, uint16_t vid)
{
	snprintf(out, len, MESH_SEG_BRIDGE "%u", vid);
}

static void seg_link_name(char *out, size_t len, uint16_t vid, unsigned int idx)
{
	snprintf(out, len, MESH_SEG_PREFIX "%u_%u", vid, idx);
}

static bool ctrl_bridge(const char *network, char *out, size_t len)
{
	struct uci_session u;
	struct uci_section *s;
	const char *dev;
	bool ok = false;

	if (!uci_session_open(&u, "network"))
		return false;

	s = uci_lookup_section(u.ctx, u.pkg, network);
	dev = s ? uci_lookup_option_string(u.ctx, s, "device") : NULL;
	if (dev) {
		snprintf(out, len, "%s", dev);
		ok = true;
	}

	uci_session_close(&u);

	return ok;
}

static void segs_node_collect(void)
{
	struct uci_session u;
	struct uci_element *e;

	if (!uci_session_open(&u, "roamd"))
		return;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *sec = uci_to_section(e);
		const char *vid = uci_lookup_option_string(u.ctx, sec, "vid");
		struct seg_link *s;

		if (strcmp(sec->type, "wifinet") || !vid || !atoi(vid) ||
		    segs_n >= ARRAY_SIZE(segs))
			continue;

		s = &segs[segs_n++];
		s->vid = atoi(vid);
		s->usable = false;
		seg_bridge_name(s->bridge, sizeof(s->bridge), s->vid);
	}

	uci_session_close(&u);
}

static unsigned int segs_collect(void)
{
	struct seg_link prev[MESH_NET_MAX];
	unsigned int prev_n = segs_n;
	const struct mesh_network *n;
	unsigned int i, j;

	memcpy(prev, segs, sizeof(prev));
	segs_n = 0;

	if (mesh.role == MESH_NODE) {
		segs_node_collect();

		for (i = 0; i < segs_n; i++)
			for (j = 0; j < prev_n; j++)
				if (prev[j].vid == segs[i].vid)
					segs[i].usable = prev[j].usable;

		return segs_n;
	}

	for (i = 0; (n = mesh_network_at(i)); i++) {
		struct seg_link *s;

		if (!n->segment || !n->vid || segs_n >= ARRAY_SIZE(segs))
			continue;

		s = &segs[segs_n++];
		s->vid = n->vid;
		s->bridge[0] = 0;
		s->usable = ctrl_bridge(n->network, s->bridge, sizeof(s->bridge));
	}

	return segs_n;
}

static struct seg_link *seg_by_vid(uint16_t vid)
{
	unsigned int i;

	for (i = 0; i < segs_n; i++)
		if (segs[i].vid == vid)
			return &segs[i];

	return NULL;
}

bool mesh_seg_usable(uint16_t vid)
{
	const struct seg_link *s = seg_by_vid(vid);

	return s && s->usable;
}

const char *mesh_seg_node_issue(void)
{
	return seg_issue;
}

const char *mesh_seg_node_trunk(void)
{
	return seg_trunk;
}

static bool cmd_out(const char *cmd, char *buf, size_t len)
{
	FILE *f = popen(cmd, "r");
	size_t got;

	if (!f)
		return false;

	got = fread(buf, 1, len - 1, f);
	buf[got] = 0;
	pclose(f);

	return got > 0;
}

struct switch_info {
	char dev[MESH_WORD_MAX];
	int cpu;
	int port;
	unsigned int links;
};

static bool switch_probe(struct switch_info *out)
{
	char list[512], cmd[64], show[8192];
	char *line, *save = NULL;
	int port = -1;

	memset(out, 0, sizeof(*out));
	out->cpu = -1;
	out->port = -1;

	if (!cmd_out(SWITCH_LIST, list, sizeof(list)) ||
	    sscanf(list, "Found: %11s", out->dev) != 1 || !out->dev[0])
		return false;

	snprintf(cmd, sizeof(cmd), SWITCH_HELP, out->dev);
	if (!cmd_out(cmd, show, sizeof(show)))
		return false;

	line = strstr(show, "(cpu @ ");
	if (!line)
		return false;

	out->cpu = atoi(line + strlen("(cpu @ "));
	save = NULL;

	snprintf(cmd, sizeof(cmd), SWITCH_SHOW, out->dev);
	if (!cmd_out(cmd, show, sizeof(show)))
		return false;

	for (line = strtok_r(show, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!strstr(line, "link:up") ||
		    sscanf(line, " link: port:%d", &port) != 1 || port == out->cpu)
			continue;

		out->links++;
		out->port = port;
	}

	return out->cpu >= 0;
}

static void base_name(const char *ifname, char *out, size_t len, bool *tagged)
{
	const char *dot = strrchr(ifname, '.');

	*tagged = dot && dot[1] && strspn(dot + 1, "0123456789") == strlen(dot + 1);

	if (*tagged)
		snprintf(out, len, "%.*s", (int)(dot - ifname), ifname);
	else
		snprintf(out, len, "%s", ifname);
}

static uint16_t seg_section_vid(const struct uci_section *s)
{
	const char *p = NULL;

	if (!strncmp(s->e.name, SEG_SECTION, strlen(SEG_SECTION)))
		p = s->e.name + strlen(SEG_SECTION);
	else if (!strcmp(s->type, "interface") && !strncmp(s->e.name, "seg", 3))
		p = s->e.name + 3;

	if (!p || !isdigit((unsigned char)*p))
		return 0;

	return atoi(p);
}

static void links_reconcile(uint16_t vid, char keep[][SEG_NAME_MAX], unsigned int n_keep)
{
	char prefix[SEG_NAME_MAX];
	struct dirent *de;
	DIR *d = opendir(NET_SYS);
	unsigned int i;

	if (!d)
		return;

	snprintf(prefix, sizeof(prefix), MESH_SEG_PREFIX "%u_", vid);

	while ((de = readdir(d))) {
		bool wanted = false;

		if (strncmp(de->d_name, prefix, strlen(prefix)))
			continue;

		for (i = 0; i < n_keep && !wanted; i++)
			wanted = !strcmp(de->d_name, keep[i]);

		if (!wanted)
			mesh_link_del(de->d_name);
	}

	closedir(d);
}

static unsigned int child_links(char out[][IFNAMSIZ], unsigned int max)
{
	struct dirent *de;
	DIR *d = opendir(NET_SYS);
	unsigned int n = 0;

	if (!d)
		return 0;

	while ((de = readdir(d)) && n < max) {
		char path[320];

		if (!strstr(de->d_name, ".sta"))
			continue;

		snprintf(path, sizeof(path), NET_SYS "/%s/phy80211", de->d_name);
		if (access(path, F_OK))
			continue;

		snprintf(out[n], IFNAMSIZ, "%.*s", IFNAMSIZ - 1, de->d_name);
		n++;
	}

	closedir(d);

	return n;
}

static bool cable_allowed(void)
{
	struct switch_info sw;

	return mesh.role == MESH_CONTROLLER || !switch_probe(&sw);
}

static unsigned int wired_ports(char out[][IFNAMSIZ], unsigned int max)
{
	char ports[MESH_BRPORT_MAX][IFNAMSIZ];
	char lan[IFNAMSIZ], base[IFNAMSIZ];
	unsigned int n = 0, total, i, j;
	bool seen, tagged;

	if (!cable_allowed() || !mesh_bridge_lan(lan, sizeof(lan)))
		return 0;

	total = mesh_bridge_ports(lan, ports, ARRAY_SIZE(ports));

	for (i = 0; i < total && n < max; i++) {
		char path[320];

		snprintf(path, sizeof(path), NET_SYS "/%s/phy80211", ports[i]);
		if (!access(path, F_OK))
			continue;

		base_name(ports[i], base, sizeof(base), &tagged);
		seen = false;

		for (j = 0; j < n && !seen; j++)
			seen = !strcmp(out[j], base);

		if (seen)
			continue;

		snprintf(out[n], IFNAMSIZ, "%.*s", IFNAMSIZ - 1, base);
		n++;
	}

	return n;
}

static void links_sweep(void)
{
	struct dirent *de;
	DIR *d = opendir(NET_SYS);
	unsigned int i;

	if (!d)
		return;

	while ((de = readdir(d))) {
		unsigned int vid = 0;
		bool known = false;

		if (strncmp(de->d_name, MESH_SEG_PREFIX, strlen(MESH_SEG_PREFIX)) ||
		    sscanf(de->d_name + strlen(MESH_SEG_PREFIX), "%u_", &vid) != 1)
			continue;

		for (i = 0; i < segs_n && !known; i++)
			known = segs[i].vid == vid && segs[i].usable;

		if (!known)
			mesh_link_del(de->d_name);
	}

	closedir(d);
}

void mesh_seg_links_sync(void)
{
	char parents[SEG_LINK_MAX][IFNAMSIZ];
	char keep[SEG_LINK_MAX][SEG_NAME_MAX];
	char sta[IFNAMSIZ];
	unsigned int i, n_parents, n_keep, p;

	if (!segs_collect()) {
		links_sweep();

		return;
	}

	n_parents = child_links(parents, ARRAY_SIZE(parents));

	if (mesh.role == MESH_NODE && mesh_bridge_sta_name(sta, sizeof(sta)) &&
	    n_parents < ARRAY_SIZE(parents)) {
		snprintf(parents[n_parents], IFNAMSIZ, "%s", sta);
		n_parents++;
	}

	n_parents += wired_ports(parents + n_parents, ARRAY_SIZE(parents) - n_parents);

	links_sweep();

	roam_log(ROAM_L_DEBUG, "mesh: segments %u, parents %u", segs_n, n_parents);

	for (i = 0; i < segs_n; i++) {
		struct seg_link *s = &segs[i];

		n_keep = 0;

		if (!s->usable || !mesh_link_exists(s->bridge)) {
			links_reconcile(s->vid, keep, 0);
			continue;
		}

		if (mesh.role == MESH_CONTROLLER)
			mesh_bridge_stp_root(s->bridge);

		for (p = 0; p < n_parents; p++) {
			unsigned int idx = if_nametoindex(parents[p]);

			if (!idx)
				continue;

			seg_link_name(keep[n_keep], SEG_NAME_MAX, s->vid, idx);

			if (mesh_link_enslave(parents[p], s->vid, keep[n_keep], s->bridge)) {
				n_keep++;
				continue;
			}

			roam_log(ROAM_L_ERR, "mesh: segment %u could not be linked over %s into %s",
				 s->vid, parents[p], s->bridge);
		}

		links_reconcile(s->vid, keep, n_keep);
	}
}

bool mesh_seg_node_apply(void)
{
	struct uci_session u;
	struct uci_element *e, *tmp;
	bool air, cable, changed;
	unsigned int i;

	seg_issue[0] = 0;
	seg_trunk = "";

	if (!segs_collect())
		return false;

	air = mesh_bridge_sta_up();
	cable = cable_allowed();

	if (!cable && !air)
		snprintf(seg_issue, sizeof(seg_issue),
			 "the switch of the node tags the whole cable, a segment only goes over the air");

	for (i = 0; i < segs_n; i++)
		segs[i].usable = air || cable;

	if (segs_n && (air || cable))
		seg_trunk = cable ? "port" : "air";

	if (!uci_session_open(&u, "network"))
		return false;

	uci_foreach_element_safe(&u.pkg->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		uint16_t vid = seg_section_vid(s);

		if (!vid || mesh_seg_usable(vid))
			continue;

		uci_session_delete(&u, s->e.name, NULL);
		links_reconcile(vid, NULL, 0);
	}

	for (i = 0; i < segs_n; i++) {
		char brname[IFNAMSIZ], iface[MESH_WORD_MAX], sect[SEG_NAME_MAX];
		uint16_t vid = segs[i].vid;

		if (!segs[i].usable)
			continue;

		seg_bridge_name(brname, sizeof(brname), vid);
		snprintf(iface, sizeof(iface), "seg%u", vid);
		snprintf(sect, sizeof(sect), SEG_SECTION "%u_br", vid);

		if (uci_session_add(&u, "device", sect)) {
			uci_session_set(&u, sect, "type", "bridge");
			uci_session_set(&u, sect, "name", brname);
			uci_session_set(&u, sect, "stp", "1");
			uci_session_set(&u, sect, "bridge_empty", "1");
		}

		if (uci_session_add(&u, "interface", iface)) {
			uci_session_set(&u, iface, "proto", "none");
			uci_session_set(&u, iface, "device", brname);
		}
	}

	changed = u.dirty;
	uci_session_close(&u);

	return changed;
}

bool mesh_seg_node_drop(void)
{
	struct uci_session u;
	struct uci_element *e, *tmp;
	bool changed;

	if (!uci_session_open(&u, "network"))
		return false;

	uci_foreach_element_safe(&u.pkg->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		uint16_t vid = seg_section_vid(s);

		if (!vid)
			continue;

		uci_session_delete(&u, s->e.name, NULL);
		links_reconcile(vid, NULL, 0);
	}

	changed = u.dirty;
	uci_session_close(&u);

	return changed;
}
