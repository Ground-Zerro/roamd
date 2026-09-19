#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <netinet/ether.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define MEMBERS_DIR	"/var/run/roamd/members"
#define POLL_STA_MAX	32
#define POLL_AP_MAX	8
#define POLL_NODE_MAX	(MESH_ALLOW_MAX + 1)
#define POLL_NR_MAX	160
#define SELF_ID		"controller"

struct poll_sta {
	char mac[MESH_MAC_MAX];
	char ssid[MESH_SSID_MAX];
	int signal;
	uint32_t connected;
	bool client;
};

struct poll_ap {
	char bssid[MESH_MAC_MAX];
	char ssid[MESH_SSID_MAX];
	char band[4];
	char nr[POLL_NR_MAX];
};

struct poll_node {
	char id[MESH_ID_MAX];
	char addr[MESH_ADDR_MAX];
	char name[MESH_NAME_MAX];
	bool managed;
	bool online;
	struct poll_sta sta[POLL_STA_MAX];
	unsigned int n_sta;
	struct poll_ap ap[POLL_AP_MAX];
	unsigned int n_ap;
};

static struct poll_node nodes[POLL_NODE_MAX];
static unsigned int n_nodes;
static unsigned int waiting;
static bool busy;
static bool force_pending;
static bool force_apply;
static char *profile_json;
static char profile_sum[MESH_NET_SUM_LEN];

bool mesh_poll_busy(void)
{
	return busy;
}

static const char *attr_str(struct blob_attr *table, const char *name)
{
	struct blob_attr *cur;
	int rem;

	if (!table)
		return NULL;

	blobmsg_for_each_attr(cur, table, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			return blobmsg_get_string(cur);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_BOOL)
			return blobmsg_get_bool(cur) ? "true" : "false";

		return NULL;
	}

	return NULL;
}

static struct blob_attr *attr_field(struct blob_attr *table, const char *name, int type)
{
	struct blob_attr *cur;
	int rem;

	if (!table)
		return NULL;

	blobmsg_for_each_attr(cur, table, rem)
		if (!strcmp(blobmsg_name(cur), name) && blobmsg_type(cur) == type)
			return cur;

	return NULL;
}

static struct blob_attr *root_field(struct blob_buf *b, const char *name, int type)
{
	struct blob_attr *cur;
	int rem;

	blob_for_each_attr(cur, b->head, rem)
		if (!strcmp(blobmsg_name(cur), name) && blobmsg_type(cur) == type)
			return cur;

	return NULL;
}

static uint32_t attr_u32(struct blob_attr *table, const char *name)
{
	struct blob_attr *cur = attr_field(table, name, BLOBMSG_TYPE_INT32);

	return cur ? blobmsg_get_u32(cur) : 0;
}

static void sta_collect(struct poll_node *n, struct blob_attr *list, bool client)
{
	struct blob_attr *cur;
	int rem;

	if (!list)
		return;

	blobmsg_for_each_attr(cur, list, rem) {
		struct poll_sta *s;
		const char *mac, *ssid;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE || n->n_sta >= POLL_STA_MAX)
			continue;

		mac = attr_str(cur, "mac");
		if (!mac || !attr_field(cur, "signal", BLOBMSG_TYPE_INT32))
			continue;

		s = &n->sta[n->n_sta++];
		memset(s, 0, sizeof(*s));
		snprintf(s->mac, sizeof(s->mac), "%s", mac);
		ssid = attr_str(cur, "ssid");
		if (ssid)
			snprintf(s->ssid, sizeof(s->ssid), "%s", ssid);
		s->signal = (int)attr_u32(cur, "signal");
		s->connected = client ? attr_u32(cur, "connected") : 0;
		s->client = client;
	}
}

static void ap_collect(struct poll_node *n, struct blob_attr *list)
{
	struct blob_attr *cur;
	int rem;

	if (!list)
		return;

	blobmsg_for_each_attr(cur, list, rem) {
		struct poll_ap *a;
		const char *bssid, *ssid, *band, *nr;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE || n->n_ap >= POLL_AP_MAX)
			continue;

		bssid = attr_str(cur, "bssid");
		ssid = attr_str(cur, "ssid");
		if (!bssid || !ssid)
			continue;

		a = &n->ap[n->n_ap++];
		memset(a, 0, sizeof(*a));
		snprintf(a->bssid, sizeof(a->bssid), "%s", bssid);
		snprintf(a->ssid, sizeof(a->ssid), "%s", ssid);
		band = attr_str(cur, "band");
		if (band)
			snprintf(a->band, sizeof(a->band), "%s", band);
		nr = attr_str(cur, "nr");
		if (nr)
			snprintf(a->nr, sizeof(a->nr), "%s", nr);
	}
}

static void member_file_write(const struct poll_node *n, struct blob_attr *report,
			      const char *consistency, const char *issues, bool update)
{
	char path[128], tmp[136], *json;
	struct blob_buf b = { 0 };
	struct blob_attr *cur;
	char *src;
	int rem;
	FILE *f;

	snprintf(path, sizeof(path), MEMBERS_DIR "/%s.json", n->id);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	blob_buf_init(&b, 0);

	if (report)
		blobmsg_for_each_attr(cur, report, rem)
			blobmsg_add_blob(&b, cur);

	blobmsg_add_u8(&b, "online", report != NULL);

	if (report) {
		char spath[128];

		blobmsg_add_u8(&b, "update_available", update);
		blobmsg_add_string(&b, "consistency", consistency);

		snprintf(spath, sizeof(spath), MEMBERS_DIR "/%s.pkgsrc", n->id);
		src = mesh_slurp(spath, 32);
		if (src) {
			src[strcspn(src, "\r\n")] = 0;
			blobmsg_add_string(&b, "pkg_source", src);
			free(src);
		}

		{
			void *arr = blobmsg_open_array(&b, "issues");

			if (issues && issues[0])
				blobmsg_add_string(&b, NULL, issues);

			blobmsg_close_array(&b, arr);
		}
	}

	json = blobmsg_format_json(b.head, true);
	blob_buf_free(&b);

	if (!json)
		return;

	f = fopen(tmp, "w");
	if (f) {
		fputs(json, f);
		fclose(f);
		rename(tmp, path);
	}

	free(json);
}

static bool member_update_available(struct blob_attr *report)
{
	const char *os = attr_str(report, "os_version");
	const char *arch = attr_str(report, "arch");
	const char *have = attr_str(report, "pkg_version");
	const char *have_ui = attr_str(report, "ui_version");
	struct mesh_pkg_meta meta;
	char branch[MESH_WORD_MAX];
	const char *dot;

	if (!os || !arch || !have)
		return false;

	dot = strrchr(os, '.');
	snprintf(branch, sizeof(branch), "%.*s", dot ? (int)(dot - os) : (int)strlen(os), os);

	if (mesh_pkg_stale(branch, arch, MESH_PKG_MAIN))
		mesh_pkg_refresh(branch, arch, MESH_PKG_MAIN, NULL, NULL);

	if (mesh_pkg_known(branch, arch, MESH_PKG_MAIN, &meta) &&
	    mesh_pkg_newer(have, meta.version) > 0)
		return true;

	if (!have_ui)
		return false;

	if (mesh_pkg_stale(branch, arch, MESH_PKG_UI))
		mesh_pkg_refresh(branch, arch, MESH_PKG_UI, NULL, NULL);

	return mesh_pkg_known(branch, arch, MESH_PKG_UI, &meta) &&
	       mesh_pkg_newer(have_ui, meta.version) > 0;
}

static void steer_round(void);

static void poll_finish(void)
{
	if (--waiting)
		return;

	steer_round();
	busy = false;
}

static void report_cb(void *priv, struct blob_attr *result, bool ok)
{
	struct poll_node *n = priv;
	struct blob_attr *profile;
	const char *nets, *bh;
	const char *issue = NULL;
	const char *cons = "ok";

	if (!ok || !result) {
		member_file_write(n, NULL, NULL, NULL, false);
		poll_finish();

		return;
	}

	n->online = true;
	sta_collect(n, attr_field(result, "clients", BLOBMSG_TYPE_ARRAY), true);
	sta_collect(n, attr_field(result, "heard", BLOBMSG_TYPE_ARRAY), false);
	ap_collect(n, attr_field(result, "aps", BLOBMSG_TYPE_ARRAY));

	profile = attr_field(result, "profile", BLOBMSG_TYPE_TABLE);
	nets = attr_str(profile, "networks_sum");
	bh = attr_str(profile, "backhaul_ssid");

	if (profile_sum[0] && (!nets || strcmp(nets, profile_sum))) {
		issue = "networks";
		cons = "broken";
	} else if (mesh.backhaul_ssid[0] && (!bh || strcmp(bh, mesh.backhaul_ssid))) {
		issue = "backhaul";
		cons = "degraded";
	}

	member_file_write(n, result, cons, issue, member_update_available(result));

	{
		struct blob_attr *events = attr_field(result, "events", BLOBMSG_TYPE_ARRAY);

		if (events && n->name[0])
			mesh_log_ingest(n->id, n->name, events);
	}

	poll_finish();
}

static void apply_cb(void *priv, struct blob_attr *result, bool ok)
{
	struct poll_node *n = priv;

	if (!mesh_rpc_call(n->id, n->addr, "roamd", "mesh_report", NULL, report_cb, n))
		report_cb(n, NULL, false);
}

static void self_collect(void)
{
	static struct blob_buf self;
	struct poll_node *n = &nodes[0];

	memset(n, 0, sizeof(*n));
	snprintf(n->id, sizeof(n->id), SELF_ID);
	snprintf(n->addr, sizeof(n->addr), "local");
	n->online = true;

	blob_buf_init(&self, 0);
	mesh_node_report(&self);

	sta_collect(n, root_field(&self, "clients", BLOBMSG_TYPE_ARRAY), true);
	sta_collect(n, root_field(&self, "heard", BLOBMSG_TYPE_ARRAY), false);
	ap_collect(n, root_field(&self, "aps", BLOBMSG_TYPE_ARRAY));
}

static bool profile_build(void)
{
	static struct blob_buf b;
	char *json;
	const char *sum;
	bool changed;

	blob_buf_init(&b, 0);
	mesh_profile_build(&b);

	sum = mesh_networks_sum();
	snprintf(profile_sum, sizeof(profile_sum), "%s", sum ? sum : "");

	json = blobmsg_format_json(b.head, true);
	if (!json)
		return false;

	changed = !profile_json || strcmp(profile_json, json);
	free(profile_json);
	profile_json = json;

	return changed;
}

void mesh_poll_run(bool force)
{
	struct mesh_member *m;

	if (mesh.role != MESH_CONTROLLER || list_empty(&mesh_members))
		return;

	if (busy) {
		force_pending = force_pending || force;

		return;
	}

	force = force || force_pending;
	force_pending = false;

	mesh_dir_ensure(MEMBERS_DIR);

	if (profile_build())
		force = true;

	force_apply = force;
	self_collect();
	n_nodes = 1;
	waiting = 1;
	busy = true;

	list_for_each_entry(m, &mesh_members, list) {
		struct poll_node *n;
		char state[MESH_WORD_MAX];
		bool was_online, was_ok, apply;

		if (n_nodes >= POLL_NODE_MAX || !m->id[0] || !m->addr[0])
			continue;

		n = &nodes[n_nodes++];
		memset(n, 0, sizeof(*n));
		snprintf(n->id, sizeof(n->id), "%s", m->id);
		snprintf(n->addr, sizeof(n->addr), "%s", m->addr);
		snprintf(n->name, sizeof(n->name), "%s", m->name);
		n->managed = m->managed;

		was_online = mesh_member_state(m->id, "online", state, sizeof(state)) &&
			     !strcmp(state, "true");
		was_ok = mesh_member_state(m->id, "consistency", state, sizeof(state)) &&
			 !strcmp(state, "ok");
		apply = n->managed && (force_apply || !was_online || !was_ok);

		waiting++;

		if (apply && profile_json &&
		    mesh_rpc_call(n->id, n->addr, "roamd", "mesh_apply", profile_json, apply_cb, n))
			continue;

		if (!mesh_rpc_call(n->id, n->addr, "roamd", "mesh_report", NULL, report_cb, n))
			report_cb(n, NULL, false);
	}

	poll_finish();
}

static bool net_roaming(const char *ssid)
{
	const struct mesh_network *net = mesh_network_by_ssid(ssid);

	return !net || net->roaming;
}

static bool node_has_net(const struct poll_node *n, const char *ssid)
{
	unsigned int i;

	for (i = 0; i < n->n_ap; i++)
		if (!strcmp(n->ap[i].ssid, ssid))
			return true;

	return false;
}

static void member_call(const struct poll_node *n, const char *method, const char *args)
{
	if (!strcmp(n->id, SELF_ID)) {
		uint32_t id;

		if (ubus_lookup_id(ubus_ctx, "roamd", &id))
			return;

		{
			static struct blob_buf b;

			blob_buf_init(&b, 0);
			if (args && blobmsg_add_json_from_string(&b, args))
				ubus_invoke(ubus_ctx, id, method, b.head, NULL, NULL, 1000);
		}

		return;
	}

	mesh_rpc_call(n->id, n->addr, "roamd", method, args, NULL, NULL);
}

static void ghosts_drop(void)
{
	unsigned int i, j, k, l;

	for (i = 0; i < n_nodes; i++)
		for (j = 0; j < nodes[i].n_sta; j++) {
			struct poll_sta *s = &nodes[i].sta[j];
			uint32_t oldest = s->connected;

			if (!s->client || !s->connected)
				continue;

			for (k = 0; k < n_nodes; k++)
				for (l = 0; l < nodes[k].n_sta; l++) {
					struct poll_sta *o = &nodes[k].sta[l];

					if (!o->client || !o->connected ||
					    strcasecmp(o->mac, s->mac))
						continue;

					if (o->connected < oldest)
						oldest = o->connected;
				}

			if (s->connected == oldest)
				continue;

			{
				char args[64];

				snprintf(args, sizeof(args), "{\"mac\":\"%s\"}", s->mac);
				member_call(&nodes[i], "mesh_drop", args);
				s->connected = 0;
			}
		}
}

static void neighbors_push(void)
{
	static struct blob_buf b;
	unsigned int i, j, count = 0;
	char *json;
	void *arr;

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "neighbors");

	for (i = 0; i < n_nodes; i++)
		for (j = 0; j < nodes[i].n_ap; j++) {
			const struct poll_ap *a = &nodes[i].ap[j];
			void *t;

			if (!a->nr[0])
				continue;

			t = blobmsg_open_table(&b, NULL);
			blobmsg_add_string(&b, "bssid", a->bssid);
			blobmsg_add_string(&b, "ssid", a->ssid);
			blobmsg_add_string(&b, "nr", a->nr);
			blobmsg_close_table(&b, t);
			count++;
		}

	blobmsg_close_array(&b, arr);

	if (!count)
		return;

	json = blobmsg_format_json(b.head, true);
	if (!json)
		return;

	for (i = 0; i < n_nodes; i++)
		if (nodes[i].online)
			member_call(&nodes[i], "mesh_neighbors", json);

	free(json);
}

static unsigned int neighbor_list(const struct poll_node *best, const char *ssid,
				  bool prefer_only, char *out, size_t len)
{
	unsigned int i, count = 0;
	size_t used = 0;

	for (i = 0; i < best->n_ap; i++) {
		const struct poll_ap *a = &best->ap[i];
		const char *want = config.prefer == PREFER_HIGH ? "5" : "2.4";

		if (!a->nr[0] || strcmp(a->ssid, ssid))
			continue;

		if (prefer_only && config.prefer != PREFER_NONE && a->band[0] &&
		    strcmp(a->band, want))
			continue;

		if (used + strlen(a->nr) + 4 >= len)
			break;

		used += snprintf(out + used, len - used, "%s\"%s\"", count ? "," : "", a->nr);
		count++;
	}

	return count;
}

static void steer_client(const char *mac, const char *ssid, unsigned int home)
{
	const struct poll_node *best = NULL;
	int best_signal = 0, home_signal = 0;
	struct ether_addr *ea = ether_aton(mac);
	char args[1024], list[768];
	unsigned int i, j;

	if (!ea || !net_roaming(ssid))
		return;

	for (i = 0; i < n_nodes; i++) {
		const struct poll_node *n = &nodes[i];

		if (!n->online || !node_has_net(n, ssid) ||
		    !roam_device_node_allowed((uint8_t *)ea, n->id))
			continue;

		for (j = 0; j < n->n_sta; j++) {
			const struct poll_sta *s = &n->sta[j];

			if (strcasecmp(s->mac, mac))
				continue;

			if (!best || s->signal > best_signal) {
				best = n;
				best_signal = s->signal;
			}

			if (i == home)
				home_signal = s->signal;
		}
	}

	if (!best || best == &nodes[home])
		return;

	if (roam_device_node_allowed((uint8_t *)ea, nodes[home].id) &&
	    best_signal - home_signal < config.rssi_diff)
		return;

	list[0] = 0;
	if (!neighbor_list(best, ssid, true, list, sizeof(list)))
		neighbor_list(best, ssid, false, list, sizeof(list));

	if (list[0])
		snprintf(args, sizeof(args), "{\"mac\":\"%s\",\"neighbors\":[%s]}", mac, list);
	else
		snprintf(args, sizeof(args), "{\"mac\":\"%s\"}", mac);

	member_call(&nodes[home], "mesh_steer", args);
}

static void steer_round(void)
{
	unsigned int i, j;

	ghosts_drop();

	for (i = 0; i < n_nodes; i++)
		for (j = 0; j < nodes[i].n_sta; j++) {
			const struct poll_sta *s = &nodes[i].sta[j];

			if (!s->client || !s->ssid[0])
				continue;

			steer_client(s->mac, s->ssid, i);
		}

	neighbors_push();
}
