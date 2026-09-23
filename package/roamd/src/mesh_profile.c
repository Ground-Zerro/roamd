#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/if.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define MEMBERS_DIR	"/var/run/roamd/members"
#define PARENT_ENTRY_MAX	(MESH_MAC_MAX + PARENT_CHAIN + 16)
#define PARENT_MAX		MESH_BH_PARENT_MAX
#define PARENT_CHAIN	160
#define DEPTH_UNSET	-1
#define DEPTH_BUSY	-2
#define DEPTH_NONE	-3

struct profile_member {
	const char *id;
	const char *via;
	bool wifi;
	bool online;
	int depth;
	char chain[PARENT_CHAIN];
	struct blob_attr *aps;
	struct blob_buf doc;
};

static const char *const global_opts[] = {
	"mobility_domain", "band_steering", "prefer_band", "fast_transition",
	"ft_over_ds", "neighbor_reports", "bss_transition"
};

static const char *const policy_opts[] = {
	"rssi_good", "rssi_low", "rssi_diff", "node_rssi_diff", "kick_rssi",
	"hold_time", "age_time", "check_time_low", "check_time_high", "poll_interval",
	"deny_time", "deny_probe", "allow_kick", "steer_retries", "beacon_req_interval",
	"log_level"
};

static const char *root_str(struct blob_buf *doc, const char *name)
{
	struct blob_attr *cur;
	int rem;

	blob_for_each_attr(cur, doc->head, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			return blobmsg_get_string(cur);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_BOOL)
			return blobmsg_get_bool(cur) ? "true" : "false";
	}

	return NULL;
}

static struct blob_attr *root_field(struct blob_buf *doc, const char *name, int type)
{
	struct blob_attr *cur;
	int rem;

	blob_for_each_attr(cur, doc->head, rem)
		if (!strcmp(blobmsg_name(cur), name) && blobmsg_type(cur) == type)
			return cur;

	return NULL;
}

static const char *json_str(struct blob_attr *doc, const char *name)
{
	struct blob_attr *cur;
	int rem;

	if (!doc)
		return NULL;

	blobmsg_for_each_attr(cur, doc, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			return blobmsg_get_string(cur);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_BOOL)
			return blobmsg_get_bool(cur) ? "true" : "false";
	}

	return NULL;
}

bool mesh_member_state(const char *id, const char *field, char *out, size_t len)
{
	char path[sizeof(MEMBERS_DIR) + MESH_ID_MAX + 8];
	struct blob_buf doc = { 0 };
	const char *v;
	bool ok = false;

	snprintf(path, sizeof(path), MEMBERS_DIR "/%s.json", id);
	blob_buf_init(&doc, 0);

	if (blobmsg_add_json_from_file(&doc, path)) {
		v = root_str(&doc, field);
		if (v) {
			snprintf(out, len, "%s", v);
			ok = true;
		}
	}

	blob_buf_free(&doc);

	return ok;
}

static bool bssid_own(const char *bssid)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list) {
		char cur[MESH_MAC_MAX];

		roam_mac_str(bss->bssid, cur, sizeof(cur));

		if (!strcasecmp(cur, bssid))
			return true;
	}

	return false;
}

static int member_by_bssid(struct profile_member *m, unsigned int n, const char *bssid)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		struct blob_attr *ap;
		int rem;

		if (!m[i].aps)
			continue;

		blobmsg_for_each_attr(ap, m[i].aps, rem) {
			const char *cur = json_str(ap, "bssid");

			if (cur && !strcasecmp(cur, bssid))
				return (int)i;
		}
	}

	return -1;
}

static int member_depth(struct profile_member *m, unsigned int n, unsigned int i)
{
	struct profile_member *cur = &m[i];
	int parent;

	if (cur->depth != DEPTH_UNSET)
		return cur->depth;

	cur->depth = DEPTH_BUSY;

	if (!cur->wifi) {
		cur->depth = 0;
		snprintf(cur->chain, sizeof(cur->chain), "%.*s", MESH_ID_MAX - 1, cur->id);

		return cur->depth;
	}

	if (!cur->via || !strcmp(cur->via, MESH_CONTROLLER_OWNER) || bssid_own(cur->via)) {
		cur->depth = 1;
		snprintf(cur->chain, sizeof(cur->chain), "%.*s", MESH_ID_MAX - 1, cur->id);

		return cur->depth;
	}

	parent = member_by_bssid(m, n, cur->via);

	if (parent >= 0 && (unsigned int)parent != i && m[parent].depth != DEPTH_BUSY &&
	    member_depth(m, n, (unsigned int)parent) >= 0) {
		char parent_chain[PARENT_CHAIN + 1];
		snprintf(parent_chain, sizeof(parent_chain), "%.*s", PARENT_CHAIN, m[parent].chain);
		cur->depth = m[parent].depth + 1;
		snprintf(cur->chain, sizeof(cur->chain), "%.*s-%.*s",
			 PARENT_CHAIN / 2, parent_chain, MESH_ID_MAX - 1, cur->id);

		return cur->depth;
	}

	cur->depth = DEPTH_NONE;

	return cur->depth;
}

static void parent_add(char *out, size_t len, const char *bssid, int depth,
		       const char *chain, const char *band)
{
	char entry[MESH_MAC_MAX + PARENT_CHAIN + 16];
	size_t used = strlen(out);
	int n;

	n = snprintf(entry, sizeof(entry), "%.17s/%d/%.*s/%.3s", bssid, depth,
		     PARENT_CHAIN - 1, chain, band);
	if (n < 0 || used + (size_t)n + 2 > len)
		return;

	if (used)
		out[used++] = ' ';

	memcpy(out + used, entry, (size_t)n + 1);
}

static void parents_own(char *out, size_t len)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list) {
		char bssid[MESH_MAC_MAX];

		if (strcmp(bss->ssid, mesh.backhaul_ssid))
			continue;

		roam_mac_str(bss->bssid, bssid, sizeof(bssid));
		parent_add(out, len, bssid, 0, "", roam_band_name(bss->band));
	}
}

static int parent_cmp(const void *a, const void *b)
{
	return strcmp(a, b);
}

static bool parent_known(const char *entry, char list[][PARENT_ENTRY_MAX], unsigned int n)
{
	size_t len = strcspn(entry, "/");
	unsigned int i;

	for (i = 0; i < n; i++)
		if (!strncasecmp(list[i], entry, len) && list[i][len] == '/')
			return true;

	return false;
}

static void parents_sorted(char *out, size_t len, char entries[][PARENT_ENTRY_MAX],
			   unsigned int count)
{
	size_t used = 0;
	unsigned int i;

	qsort(entries, count, PARENT_ENTRY_MAX, parent_cmp);
	out[0] = '\0';

	for (i = 0; i < count; i++) {
		int n = snprintf(out + used, len - used, "%s%s", used ? " " : "", entries[i]);

		if (n < 0 || (size_t)n >= len - used)
			break;

		used += n;
	}
}

bool mesh_parent_reachable(const char *member_id, const char *nodes)
{
	char list[MESH_BH_PARENTS_LEN], allow[MESH_BH_ALLOW_LEN];
	char *tok, *save = NULL;

	if (!nodes || !nodes[0] || !mesh.backhaul_parents[0])
		return true;

	snprintf(list, sizeof(list), "%s", mesh.backhaul_parents);

	for (tok = strtok_r(list, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		char *depth = strchr(tok, '/');
		char *chain = depth ? strchr(depth + 1, '/') : NULL;
		char *band = chain ? strchr(chain + 1, '/') : NULL;
		const char *owner, *dash;
		char *asave = NULL, *atok;

		if (!band)
			continue;

		*chain++ = '\0';
		*band = '\0';

		if (mesh_chain_has(chain, member_id))
			continue;

		dash = strrchr(chain, '-');
		owner = dash ? dash + 1 : (chain[0] ? chain : MESH_CONTROLLER_OWNER);

		snprintf(allow, sizeof(allow), "%s", nodes);

		for (atok = strtok_r(allow, "-", &asave); atok;
		     atok = strtok_r(NULL, "-", &asave))
			if (!strcmp(atok, owner))
				return true;
	}

	return false;
}

static void parents_build(char *out, size_t len)
{
	struct profile_member list[MESH_ALLOW_MAX];
	struct mesh_member *m;
	unsigned int n = 0, i;

	out[0] = 0;

	if (!mesh.backhaul_ssid[0])
		return;

	parents_own(out, len);

	list_for_each_entry(m, &mesh_members, list) {
		char path[sizeof(MEMBERS_DIR) + MESH_ID_MAX + 8];
		const char *online, *conn;
		struct profile_member *p;

		if (n >= ARRAY_SIZE(list))
			break;

		p = &list[n];
		memset(p, 0, sizeof(*p));

		snprintf(path, sizeof(path), MEMBERS_DIR "/%s.json", m->id);
		blob_buf_init(&p->doc, 0);

		if (!blobmsg_add_json_from_file(&p->doc, path)) {
			blob_buf_free(&p->doc);
			continue;
		}

		p->id = m->id;
		online = root_str(&p->doc, "online");
		if (!online || strcmp(online, "true")) {
			blob_buf_free(&p->doc);
			continue;
		}

		p->online = true;
		p->via = root_str(&p->doc, "via");
		conn = root_str(&p->doc, "connection");
		p->wifi = conn && !strcmp(conn, "wifi");
		p->aps = root_field(&p->doc, "aps", BLOBMSG_TYPE_ARRAY);
		p->depth = DEPTH_UNSET;
		n++;
	}

	for (i = 0; i < n; i++) {
		struct blob_attr *ap;
		int rem;

		if (member_depth(list, n, i) < 0 || !list[i].aps)
			continue;

		blobmsg_for_each_attr(ap, list[i].aps, rem) {
			const char *ssid = json_str(ap, "ssid");
			const char *bssid = json_str(ap, "bssid");
			const char *band = json_str(ap, "band");

			if (!ssid || !bssid || !band || strcmp(ssid, mesh.backhaul_ssid))
				continue;

			parent_add(out, len, bssid, list[i].depth, list[i].chain, band);
		}
	}

	for (i = 0; i < n; i++)
		blob_buf_free(&list[i].doc);

	{
		char entries[PARENT_MAX][PARENT_ENTRY_MAX];
		char kept[MESH_BH_PARENTS_LEN];
		unsigned int count = 0;
		char *tok, *save = NULL;

		snprintf(kept, sizeof(kept), "%s", out);

		for (tok = strtok_r(kept, " ", &save); tok && count < PARENT_MAX;
		     tok = strtok_r(NULL, " ", &save))
			snprintf(entries[count++], PARENT_ENTRY_MAX, "%s", tok);

		snprintf(kept, sizeof(kept), "%s", mesh.backhaul_parents);

		for (tok = strtok_r(kept, " ", &save); tok && count < PARENT_MAX;
		     tok = strtok_r(NULL, " ", &save))
			if (!parent_known(tok, entries, count))
				snprintf(entries[count++], PARENT_ENTRY_MAX, "%s", tok);

		parents_sorted(out, len, entries, count);
	}
}

static void opts_dump(struct blob_buf *b, const char *name,
		      const char *const *opts, size_t n)
{
	void *t = blobmsg_open_table(b, name);
	char value[64];
	size_t i;

	for (i = 0; i < n; i++)
		if (roam_config_value(opts[i], value, sizeof(value)))
			blobmsg_add_string(b, opts[i], value);

	blobmsg_close_table(b, t);
}

static void system_dump(struct blob_buf *b)
{
	static const char *const opts[] = { "timezone", "zonename" };
	void *t = blobmsg_open_table(b, "system");
	struct uci_session u;
	struct uci_element *e;
	size_t i;

	if (uci_session_open(&u, "system")) {
		uci_foreach_element(&u.pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);

			if (strcmp(s->type, "system"))
				continue;

			for (i = 0; i < ARRAY_SIZE(opts); i++) {
				const char *v = uci_lookup_option_string(u.ctx, s, opts[i]);

				if (v)
					blobmsg_add_string(b, opts[i], v);
			}
			break;
		}

		uci_session_close(&u);
	}

	blobmsg_close_table(b, t);
}

static void credentials_dump(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "credentials");
	char *shadow = mesh_slurp("/etc/shadow", 8192);
	char *line, *save = NULL;

	for (line = shadow ? strtok_r(shadow, "\n", &save) : NULL; line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *hash, *end;

		if (strncmp(line, "root:", 5))
			continue;

		hash = line + 5;
		end = strchr(hash, ':');
		if (end)
			*end = 0;

		if (hash[0] && strcmp(hash, "x"))
			blobmsg_add_string(b, "root_hash", hash);
		break;
	}

	free(shadow);

	{
		char pubkey[MESH_PUBKEY_MAX];
		unsigned int k;

		for (k = 0; k < __MESH_KEY_MAX; k++)
			if (mesh_ssh_pubkey(k, pubkey, sizeof(pubkey)))
				blobmsg_add_string(b, mesh_key_fields[k], pubkey);
	}

	blobmsg_close_table(b, t);
}

static void devices_dump(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "devices");
	struct uci_session u;
	struct uci_element *e;

	if (uci_session_open(&u, "roamd")) {
		uci_foreach_element(&u.pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);
			const char *mac = uci_lookup_option_string(u.ctx, s, "mac");
			const char *band = uci_lookup_option_string(u.ctx, s, "band");
			struct uci_option *nodes;
			void *t, *na;

			if (strcmp(s->type, "device") || !mac)
				continue;

			t = blobmsg_open_table(b, NULL);
			blobmsg_add_string(b, "mac", mac);
			if (band && mesh_band_usable(mesh_band_bit(band)))
				blobmsg_add_string(b, "band", band);

			na = blobmsg_open_array(b, "nodes");
			nodes = uci_lookup_option(u.ctx, s, "node");
			if (nodes && nodes->type == UCI_TYPE_LIST) {
				struct uci_element *le;

				uci_foreach_element(&nodes->v.list, le)
					blobmsg_add_string(b, NULL, le->name);
			}
			blobmsg_close_array(b, na);
			blobmsg_close_table(b, t);
		}

		uci_session_close(&u);
	}

	blobmsg_close_array(b, arr);
}

void mesh_profile_build(struct blob_buf *b)
{
	char parents[MESH_BH_PARENTS_LEN];
	char value[16];
	void *t;

	mesh_networks_collect();

	blobmsg_add_string(b, "controller_id", mesh.controller_id);
	blobmsg_add_string(b, "networks_sum", mesh_networks_sum());
	mesh_networks_dump(b, "networks");

	system_dump(b);
	credentials_dump(b);
	opts_dump(b, "global", global_opts, ARRAY_SIZE(global_opts));
	opts_dump(b, "policy", policy_opts, ARRAY_SIZE(policy_opts));

	t = blobmsg_open_table(b, "backhaul");
	blobmsg_add_string(b, "backhaul_enabled", mesh.backhaul_enabled ? "1" : "0");
	blobmsg_add_string(b, "backhaul_ssid", mesh.backhaul_ssid);
	blobmsg_add_string(b, "backhaul_key", mesh.backhaul_key);
	blobmsg_add_string(b, "ft_key", mesh.ft_key);
	blobmsg_add_string(b, "ft_kh", mesh.ft_kh);
	blobmsg_add_string(b, "wifi_shutdown", mesh.wifi_shutdown ? "1" : "0");
	blobmsg_add_string(b, "node_ui", mesh.node_ui ? "1" : "0");

	{
		char lan[IFNAMSIZ], path[64], *mac;

		if (mesh_bridge_lan(lan, sizeof(lan))) {
			snprintf(path, sizeof(path), "/sys/class/net/%s/address", lan);
			mac = mesh_slurp(path, 32);

			if (mac) {
				mac[strcspn(mac, "\r\n")] = 0;
				blobmsg_add_string(b, "controller_mac", mac);
				free(mac);
			}
		}
	}
	snprintf(value, sizeof(value), "%d", mesh.backhaul_delta);
	blobmsg_add_string(b, "backhaul_delta", value);
	snprintf(value, sizeof(value), "%d", mesh.backhaul_min_signal);
	blobmsg_add_string(b, "backhaul_min_signal", value);

	parents_build(parents, sizeof(parents));
	if (parents[0])
		blobmsg_add_string(b, "backhaul_parents", parents);

	{
		char limits[MESH_BH_LIMITS_LEN] = "";
		struct mesh_member *m;
		size_t used = 0;

		list_for_each_entry(m, &mesh_members, list) {
			int n;

			if (!m->bh_nodes[0])
				continue;

			n = snprintf(limits + used, sizeof(limits) - used, "%s%s/%s",
				     used ? " " : "", m->id, m->bh_nodes);

			if (n < 0 || (size_t)n >= sizeof(limits) - used)
				break;

			used += (size_t)n;
		}

		blobmsg_add_string(b, "bh_limits", limits);
	}
	blobmsg_close_table(b, t);

	devices_dump(b);
}
