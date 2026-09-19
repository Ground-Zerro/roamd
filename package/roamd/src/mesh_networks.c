#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg.h>

#include "roamd.h"
#include "mesh.h"

#define NET_SECTION	"roamd_net_"
#define MESH_SEC_MAX	32
#define RADIO_MAX	4

struct net_radio {
	char name[MESH_WORD_MAX];
	uint8_t band;
};

static struct mesh_network nets[MESH_NET_MAX];
static unsigned int nets_n;
static char nets_sum[MESH_NET_SUM_LEN];

static uint8_t band_of(struct uci_context *ctx, struct uci_section *s)
{
	const char *band = uci_lookup_option_string(ctx, s, "band");
	const char *hwmode = uci_lookup_option_string(ctx, s, "hwmode");
	const char *channel = uci_lookup_option_string(ctx, s, "channel");

	if (band)
		return band[0] == '2' ? MESH_BAND_24 : MESH_BAND_5;

	if (hwmode)
		return strstr(hwmode, "g") && !strstr(hwmode, "a") ? MESH_BAND_24 : MESH_BAND_5;

	if (channel && atoi(channel) > 14)
		return MESH_BAND_5;

	return MESH_BAND_24;
}

static unsigned int radios_collect(struct uci_session *u, struct net_radio *out, unsigned int max)
{
	struct uci_element *e;
	unsigned int n = 0;

	uci_foreach_element(&u->pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (strcmp(s->type, "wifi-device") || n >= max)
			continue;

		snprintf(out[n].name, sizeof(out[n].name), "%s", s->e.name);
		out[n].band = band_of(u->ctx, s);
		n++;
	}

	return n;
}

static struct mesh_network *net_find(const char *ssid)
{
	unsigned int i;

	for (i = 0; i < nets_n; i++)
		if (!strcmp(nets[i].ssid, ssid))
			return &nets[i];

	return NULL;
}

static struct mesh_network *net_add(const char *ssid)
{
	struct mesh_network *n = net_find(ssid);

	if (n || nets_n >= MESH_NET_MAX)
		return n;

	n = &nets[nets_n++];
	memset(n, 0, sizeof(*n));
	snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
	snprintf(n->id, sizeof(n->id), "%04x", roam_hash16(ssid));
	n->roaming = true;

	return n;
}

static void net_flags(const char *ssid, bool *roaming, uint16_t *vid)
{
	struct uci_session u;
	struct uci_element *e;

	*roaming = true;
	*vid = 0;

	if (!uci_session_open(&u, "roamd"))
		return;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *name = uci_lookup_option_string(u.ctx, s, "ssid");
		const char *v;

		if (strcmp(s->type, "wifinet") || !name || strcmp(name, ssid))
			continue;

		v = uci_lookup_option_string(u.ctx, s, "roaming");
		*roaming = v ? roam_uci_bool(v) : true;
		v = uci_lookup_option_string(u.ctx, s, "vid");
		if (v)
			*vid = atoi(v);
		break;
	}

	uci_session_close(&u);
}

static bool vid_taken(struct uci_session *u, uint16_t vid)
{
	struct uci_element *e;
	char suffix[8];

	snprintf(suffix, sizeof(suffix), ".%u", vid);

	uci_foreach_element(&u->pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *name = uci_lookup_option_string(u->ctx, s, "name");
		const char *v = uci_lookup_option_string(u->ctx, s, "vid");
		const char *vlan = uci_lookup_option_string(u->ctx, s, "vlan");
		size_t len;

		if (v && atoi(v) == vid)
			return true;

		if (vlan && atoi(vlan) == vid)
			return true;

		if (!name)
			continue;

		len = strlen(name);
		if (len > strlen(suffix) && !strcmp(name + len - strlen(suffix), suffix))
			return true;
	}

	return false;
}

static uint16_t vid_alloc(void)
{
	struct uci_session u;
	uint16_t vid;
	unsigned int i;

	if (!uci_session_open(&u, "network"))
		return 0;

	for (vid = MESH_SEG_VID_MIN; vid <= MESH_SEG_VID_MAX; vid++) {
		bool used = vid_taken(&u, vid);

		for (i = 0; !used && i < nets_n; i++)
			used = nets[i].vid == vid;

		if (used)
			continue;

		uci_session_close(&u);

		return vid;
	}

	uci_session_close(&u);

	return 0;
}

struct known_ssids {
	char ssid[MESH_NET_MAX * 2][MESH_SSID_MAX];
	unsigned int n;
};

static void known_add(struct known_ssids *k, const char *ssid)
{
	unsigned int i;

	for (i = 0; i < k->n; i++)
		if (!strcmp(k->ssid[i], ssid))
			return;

	if (k->n < ARRAY_SIZE(k->ssid))
		snprintf(k->ssid[k->n++], MESH_SSID_MAX, "%s", ssid);
}

static void nets_prune(const struct known_ssids *k)
{
	char drop[MESH_NET_MAX * 2][MESH_SEC_MAX];
	unsigned int n_drop = 0, i;
	struct uci_session u;
	struct uci_element *e;

	if (!uci_session_open(&u, "roamd"))
		return;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *ssid = uci_lookup_option_string(u.ctx, s, "ssid");
		bool keep = false;

		if (strcmp(s->type, "wifinet") || !ssid ||
		    strncmp(s->e.name, NET_SECTION, strlen(NET_SECTION)))
			continue;

		for (i = 0; i < k->n && !keep; i++)
			keep = !strcmp(k->ssid[i], ssid);

		if (!keep && n_drop < ARRAY_SIZE(drop))
			snprintf(drop[n_drop++], MESH_SEC_MAX, "%s", s->e.name);
	}

	for (i = 0; i < n_drop; i++)
		uci_session_delete(&u, drop[i], NULL);

	uci_session_close(&u);
}

static void vid_assign(void)
{
	struct uci_session u;
	unsigned int i;

	if (!uci_session_open(&u, "roamd"))
		return;

	for (i = 0; i < nets_n; i++) {
		struct mesh_network *n = &nets[i];
		struct uci_section *sec;
		char name[32];
		const char *cur;

		if (!n->segment) {
			n->vid = 0;
			continue;
		}

		snprintf(name, sizeof(name), NET_SECTION "%.4s", n->id);
		sec = uci_lookup_section(u.ctx, u.pkg, name);
		cur = sec ? uci_lookup_option_string(u.ctx, sec, "vid") : NULL;

		if (n->vid)
			continue;

		n->vid = cur ? atoi(cur) : 0;
		if (n->vid)
			continue;

		n->vid = vid_alloc();
		if (!n->vid)
			continue;

		if (uci_session_add(&u, "wifinet", name)) {
			char value[8];

			snprintf(value, sizeof(value), "%u", n->vid);
			uci_session_set(&u, name, "ssid", n->ssid);
			uci_session_set(&u, name, "vid", value);
		}
	}

	uci_session_close(&u);
}

static void nets_digest(void)
{
	uint32_t hash = 2166136261u;
	unsigned int i;

	for (i = 0; i < nets_n; i++) {
		const struct mesh_network *n = &nets[i];
		char line[MESH_SSID_MAX + MESH_KEY_MAX + 48];
		const char *p;
		int len;

		len = snprintf(line, sizeof(line), "%s|%s|%s|%u|%u|%u|%u|%u", n->ssid, n->encryption,
			       n->key, n->bands, n->roaming, n->hidden, n->isolate, n->vid);
		for (p = line; p < line + len; p++) {
			hash ^= (uint8_t)*p;
			hash *= 16777619u;
		}
	}

	snprintf(nets_sum, sizeof(nets_sum), "%08x", hash);
}

unsigned int mesh_networks_collect(void)
{
	struct net_radio radios[RADIO_MAX];
	struct known_ssids known = { 0 };
	struct uci_session u;
	struct uci_element *e;
	unsigned int nradios, i;

	nets_n = 0;

	if (!uci_session_open(&u, "wireless"))
		return 0;

	nradios = radios_collect(&u, radios, ARRAY_SIZE(radios));

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *mode = uci_lookup_option_string(u.ctx, s, "mode");
		const char *ssid = uci_lookup_option_string(u.ctx, s, "ssid");
		const char *device = uci_lookup_option_string(u.ctx, s, "device");
		const char *enc = uci_lookup_option_string(u.ctx, s, "encryption");
		const char *key = uci_lookup_option_string(u.ctx, s, "key");
		const char *net = uci_lookup_option_string(u.ctx, s, "network");
		struct mesh_network *n;

		if (strcmp(s->type, "wifi-iface") || !mode || strcmp(mode, "ap") || !ssid || !ssid[0])
			continue;

		if (!strncmp(s->e.name, MESH_BH_PREFIX, strlen(MESH_BH_PREFIX)))
			continue;

		known_add(&known, ssid);

		if (roam_uci_bool(uci_lookup_option_string(u.ctx, s, "disabled")))
			continue;

		n = net_add(ssid);
		if (!n)
			continue;

		if (!n->encryption[0])
			snprintf(n->encryption, sizeof(n->encryption), "%s", enc ? enc : "none");
		if (!n->key[0] && key)
			snprintf(n->key, sizeof(n->key), "%s", key);
		if (!n->network[0])
			snprintf(n->network, sizeof(n->network), "%s", net ? net : "lan");

		n->hidden |= roam_uci_bool(uci_lookup_option_string(u.ctx, s, "hidden"));
		n->isolate |= roam_uci_bool(uci_lookup_option_string(u.ctx, s, "isolate"));

		for (i = 0; i < nradios; i++)
			if (device && !strcmp(device, radios[i].name))
				n->bands |= radios[i].band;
	}

	uci_session_close(&u);

	for (i = 0; i < nets_n; i++) {
		net_flags(nets[i].ssid, &nets[i].roaming, &nets[i].vid);
		nets[i].segment = strcmp(nets[i].network, "lan") != 0;
	}

	if (mesh.role == MESH_CONTROLLER) {
		nets_prune(&known);
		vid_assign();
	}

	nets_digest();

	return nets_n;
}

const struct mesh_network *mesh_network_at(unsigned int i)
{
	return i < nets_n ? &nets[i] : NULL;
}

const char *mesh_networks_sum(void)
{
	return nets_sum;
}

const struct mesh_network *mesh_network_by_ssid(const char *ssid)
{
	return ssid ? net_find(ssid) : NULL;
}

void mesh_networks_dump(struct blob_buf *b, const char *name)
{
	void *arr = blobmsg_open_array(b, name);
	unsigned int i;

	for (i = 0; i < nets_n; i++) {
		const struct mesh_network *n = &nets[i];
		void *t = blobmsg_open_table(b, NULL);

		blobmsg_add_string(b, "id", n->id);
		blobmsg_add_string(b, "ssid", n->ssid);
		blobmsg_add_string(b, "encryption", n->encryption);
		blobmsg_add_string(b, "key", n->key);
		blobmsg_add_string(b, "network", n->network);
		blobmsg_add_u32(b, "bands", n->bands);
		blobmsg_add_u8(b, "roaming", n->roaming);
		blobmsg_add_u8(b, "hidden", n->hidden);
		blobmsg_add_u8(b, "isolate", n->isolate);
		blobmsg_add_u8(b, "segment", n->segment);
		blobmsg_add_u32(b, "vid", n->vid);
		blobmsg_close_table(b, t);
	}

	blobmsg_close_array(b, arr);
}

enum {
	NET_ID, NET_SSID, NET_ENC, NET_KEY, NET_NETWORK,
	NET_BANDS, NET_ROAMING, NET_HIDDEN, NET_ISOLATE, NET_SEGMENT, NET_VID, __NET_MAX
};

static const struct blobmsg_policy net_policy[__NET_MAX] = {
	[NET_ID] = { .name = "id", .type = BLOBMSG_TYPE_STRING },
	[NET_SSID] = { .name = "ssid", .type = BLOBMSG_TYPE_STRING },
	[NET_ENC] = { .name = "encryption", .type = BLOBMSG_TYPE_STRING },
	[NET_KEY] = { .name = "key", .type = BLOBMSG_TYPE_STRING },
	[NET_NETWORK] = { .name = "network", .type = BLOBMSG_TYPE_STRING },
	[NET_BANDS] = { .name = "bands", .type = BLOBMSG_TYPE_INT32 },
	[NET_ROAMING] = { .name = "roaming", .type = BLOBMSG_TYPE_INT8 },
	[NET_HIDDEN] = { .name = "hidden", .type = BLOBMSG_TYPE_INT8 },
	[NET_ISOLATE] = { .name = "isolate", .type = BLOBMSG_TYPE_INT8 },
	[NET_SEGMENT] = { .name = "segment", .type = BLOBMSG_TYPE_INT8 },
	[NET_VID] = { .name = "vid", .type = BLOBMSG_TYPE_INT32 },
};

static unsigned int nets_parse(struct blob_attr *networks)
{
	struct blob_attr *cur;
	int rem;

	nets_n = 0;

	blobmsg_for_each_attr(cur, networks, rem) {
		struct blob_attr *tb[__NET_MAX];
		struct mesh_network *n;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE || nets_n >= MESH_NET_MAX)
			continue;

		blobmsg_parse(net_policy, __NET_MAX, tb, blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[NET_SSID] || !tb[NET_BANDS])
			continue;

		n = &nets[nets_n++];
		memset(n, 0, sizeof(*n));
		snprintf(n->ssid, sizeof(n->ssid), "%s", blobmsg_get_string(tb[NET_SSID]));
		snprintf(n->id, sizeof(n->id), "%s",
			 tb[NET_ID] ? blobmsg_get_string(tb[NET_ID]) : "");
		snprintf(n->encryption, sizeof(n->encryption), "%s",
			 tb[NET_ENC] ? blobmsg_get_string(tb[NET_ENC]) : "none");
		snprintf(n->key, sizeof(n->key), "%s", tb[NET_KEY] ? blobmsg_get_string(tb[NET_KEY]) : "");
		snprintf(n->network, sizeof(n->network), "%s",
			 tb[NET_NETWORK] ? blobmsg_get_string(tb[NET_NETWORK]) : "lan");
		n->bands = blobmsg_get_u32(tb[NET_BANDS]);
		n->roaming = !tb[NET_ROAMING] || blobmsg_get_u8(tb[NET_ROAMING]);
		n->hidden = tb[NET_HIDDEN] && blobmsg_get_u8(tb[NET_HIDDEN]);
		n->isolate = tb[NET_ISOLATE] && blobmsg_get_u8(tb[NET_ISOLATE]);
		n->segment = tb[NET_SEGMENT] && blobmsg_get_u8(tb[NET_SEGMENT]);
		n->vid = tb[NET_VID] ? blobmsg_get_u32(tb[NET_VID]) : 0;

		if (!n->id[0])
			snprintf(n->id, sizeof(n->id), "%04x", roam_hash16(n->ssid));
	}

	nets_digest();

	return nets_n;
}

static void nets_store(void)
{
	struct uci_session u;
	struct uci_element *e, *tmp;
	unsigned int i;

	if (!uci_session_open(&u, "roamd"))
		return;

	uci_session_set(&u, "mesh", "networks_sum", nets_sum);
	snprintf(mesh.networks_sum, sizeof(mesh.networks_sum), "%s", nets_sum);

	uci_foreach_element_safe(&u.pkg->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "wifinet"))
			uci_session_delete(&u, s->e.name, NULL);
	}

	for (i = 0; i < nets_n; i++) {
		char name[32];

		snprintf(name, sizeof(name), NET_SECTION "%.4s", nets[i].id);
		if (!uci_session_add(&u, "wifinet", name))
			continue;

		uci_session_set(&u, name, "ssid", nets[i].ssid);
		uci_session_set(&u, name, "roaming", nets[i].roaming ? "1" : "0");

		if (nets[i].vid) {
			char value[8];

			snprintf(value, sizeof(value), "%u", nets[i].vid);
			uci_session_set(&u, name, "vid", value);
		}
	}

	uci_session_close(&u);
}

bool mesh_network_flag(const char *ssid, bool roaming)
{
	struct uci_session u;
	char name[32];

	if (!ssid || !ssid[0] || !uci_session_open(&u, "roamd"))
		return false;

	snprintf(name, sizeof(name), NET_SECTION "%04x", roam_hash16(ssid));

	if (uci_session_add(&u, "wifinet", name)) {
		uci_session_set(&u, name, "ssid", ssid);
		uci_session_set(&u, name, "roaming", roaming ? "1" : "0");
	}

	uci_session_close(&u);
	mesh_networks_collect();
	roam_wireless_apply();
	roam_bss_recheck();

	return true;
}

static bool iface_wanted(const char *name)
{
	unsigned int i;

	if (strncmp(name, NET_SECTION, strlen(NET_SECTION)))
		return false;

	for (i = 0; i < nets_n; i++) {
		char want[40];

		if (nets[i].segment && !mesh_seg_usable(nets[i].vid))
			continue;

		snprintf(want, sizeof(want), NET_SECTION "%.4s_", nets[i].id);
		if (!strncmp(name, want, strlen(want)))
			return true;
	}

	return false;
}

bool mesh_networks_apply(struct blob_attr *networks)
{
	struct net_radio radios[RADIO_MAX];
	struct uci_session u;
	struct uci_element *e, *tmp;
	unsigned int nradios, i, r;
	bool changed, net_changed = false;

	if (!networks || !nets_parse(networks))
		return false;

	if (mesh_seg_node_apply())
		net_changed = true;

	if (!uci_session_open(&u, "wireless"))
		return false;

	nradios = radios_collect(&u, radios, ARRAY_SIZE(radios));

	uci_foreach_element_safe(&u.pkg->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		const char *mode = uci_lookup_option_string(u.ctx, s, "mode");

		if (strcmp(s->type, "wifi-device")) {
			if (!mode || strcmp(mode, "ap") ||
			    !strncmp(s->e.name, MESH_BH_PREFIX, strlen(MESH_BH_PREFIX)) ||
			    iface_wanted(s->e.name))
				continue;

			uci_session_delete(&u, s->e.name, NULL);
			continue;
		}

		if (roam_uci_bool(uci_lookup_option_string(u.ctx, s, "disabled")))
			uci_session_set(&u, s->e.name, "disabled", "0");
	}

	for (i = 0; i < nets_n; i++) {
		const struct mesh_network *n = &nets[i];

		if (n->segment && !mesh_seg_usable(n->vid))
			continue;

		for (r = 0; r < nradios; r++) {
			char name[40];

			if (!(n->bands & radios[r].band))
				continue;

			snprintf(name, sizeof(name), NET_SECTION "%.4s_%.11s", n->id, radios[r].name);
			if (!uci_session_add(&u, "wifi-iface", name))
				continue;

			char netname[MESH_WORD_MAX];

			if (n->segment)
				snprintf(netname, sizeof(netname), "seg%u", n->vid);
			else
				snprintf(netname, sizeof(netname), "lan");

			uci_session_set(&u, name, "device", radios[r].name);
			uci_session_set(&u, name, "mode", "ap");
			uci_session_set(&u, name, "network", netname);
			uci_session_set(&u, name, "ssid", n->ssid);
			uci_session_set(&u, name, "encryption", n->encryption);
			uci_session_set(&u, name, "hidden", n->hidden ? "1" : "0");
			uci_session_set(&u, name, "isolate", n->isolate ? "1" : "0");
			uci_session_set(&u, name, "disabled", "0");

			if (n->key[0])
				uci_session_set(&u, name, "key", n->key);
			else
				uci_session_delete(&u, name, "key");
		}
	}

	changed = u.dirty;
	uci_session_close(&u);

	nets_store();
	mesh_networks_collect();
	mesh_seg_links_sync();

	return changed || net_changed;
}
