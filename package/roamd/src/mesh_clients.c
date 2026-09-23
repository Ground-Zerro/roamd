#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <netinet/ether.h>
#include <sys/stat.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define MESH_CLIENT_MAX	256
#define MEMBERS_DIR	"/var/run/roamd/members"

struct client_rec {
	uint8_t addr[6];
	char mac[18];
	char node[MESH_ID_MAX];
	char band[4];
	char std[8];
	char enc[16];
	uint32_t rate;
	uint32_t width;
	uint32_t nss;
	uint32_t connected;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint32_t last_seen;
	int sig[BAND_MAX];
	uint32_t sig_age[BAND_MAX];
	uint8_t state;
	bool wired;
	bool used;
	char host[MESH_NAME_MAX];
	char ip[16];
};

static uint64_t num_field(struct blob_attr *table, const char *name)
{
	struct blob_attr *pos;
	size_t rem;

	blobmsg_for_each_attr(pos, table, rem) {
		if (strcmp(blobmsg_name(pos), name))
			continue;

		switch (blobmsg_type(pos)) {
		case BLOBMSG_TYPE_INT64: return blobmsg_get_u64(pos);
		case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(pos);
		case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(pos);
		case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(pos);
		case BLOBMSG_TYPE_DOUBLE: return (uint64_t)blobmsg_get_double(pos);
		}
		break;
	}

	return 0;
}

enum client_state {
	CLIENT_UNKNOWN,
	CLIENT_OFFLINE,
	CLIENT_ONLINE
};

static const char *const state_name[] = {
	[CLIENT_UNKNOWN] = "unknown",
	[CLIENT_OFFLINE] = "offline",
	[CLIENT_ONLINE] = "online"
};

static struct client_rec clients[MESH_CLIENT_MAX];

static bool mac_skip(const uint8_t *addr);

static bool clients_dirty;

static struct client_rec *client_upsert(const uint8_t *addr, const char *mac)
{
	struct client_rec *free_slot = NULL;
	struct client_rec *oldest = NULL;
	unsigned int i;

	for (i = 0; i < MESH_CLIENT_MAX; i++) {
		if (clients[i].used) {
			if (!memcmp(clients[i].addr, addr, 6))
				return &clients[i];
			if (clients[i].state != CLIENT_ONLINE &&
			    (!oldest || clients[i].last_seen < oldest->last_seen))
				oldest = &clients[i];
		} else if (!free_slot) {
			free_slot = &clients[i];
		}
	}

	if (!free_slot)
		free_slot = oldest;

	if (!free_slot)
		return NULL;

	clients_dirty = true;

	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	memcpy(free_slot->addr, addr, 6);
	strncpy(free_slot->mac, mac, sizeof(free_slot->mac) - 1);

	return free_slot;
}

static void client_touch(struct client_rec *c, const char *node, const char *band,
			 uint32_t rate, const char *std, uint32_t width, uint32_t nss,
			 uint64_t rx, uint64_t tx, uint32_t now)
{
	strncpy(c->node, node, sizeof(c->node) - 1);
	c->node[sizeof(c->node) - 1] = '\0';
	c->band[0] = '\0';
	if (band)
		strncpy(c->band, band, sizeof(c->band) - 1);
	c->std[0] = '\0';
	if (std)
		strncpy(c->std, std, sizeof(c->std) - 1);
	c->wired = false;
	c->connected = 0;
	c->rate = rate;
	c->width = width;
	c->nss = nss;
	c->rx_bytes = rx;
	c->tx_bytes = tx;
	c->last_seen = now;
	c->sig[BAND_LOW] = ROAMD_NO_SIGNAL;
	c->sig[BAND_HIGH] = ROAMD_NO_SIGNAL;
	c->state = CLIENT_ONLINE;
}

static bool client_settled(const struct client_rec *c)
{
	if (c->state != CLIENT_ONLINE || !c->band[0])
		return false;

	return roam_admit(c->addr, c->node,
			  mesh_band_from_bit(mesh_band_bit(c->band))) == ADMIT_OK;
}

static void node_settle(const char *node)
{
	unsigned int i;

	for (i = 0; i < MESH_CLIENT_MAX; i++)
		if (clients[i].used && clients[i].state == CLIENT_UNKNOWN &&
		    !strcmp(clients[i].node, node))
			clients[i].state = CLIENT_OFFLINE;
}

static bool client_claimable(const struct client_rec *c, uint32_t connected)
{
	return c->state != CLIENT_ONLINE ||
	       (connected && (!c->connected || connected < c->connected));
}

static void refresh_local(uint32_t now)
{
	struct mesh_assoc_idx idx;
	uint8_t pmac[6];
	unsigned int i;

	mesh_assoc_collect(&idx);
	mesh_wired_collect(&idx, mesh_parent_mac(pmac) ? pmac : NULL);

	for (i = 0; i < idx.n; i++) {
		struct mesh_assoc *a = &idx.e[i];
		char mac[18];
		struct client_rec *c;

		if (mac_skip(a->addr))
			continue;

		roam_mac_str(a->addr, mac, sizeof(mac));

		c = client_upsert(a->addr, mac);
		if (!c || !client_claimable(c, a->connected))
			continue;

		client_touch(c, "controller", a->wired ? NULL : roam_band_name(a->band),
			     a->rate, a->std, a->width, a->nss, a->rx_bytes, a->tx_bytes, now);

		if (!a->wired) {
			struct roam_sta *sta = roam_sta_get(a->addr, false);
			enum roam_band band;

			for (band = 0; sta && band < BAND_MAX; band++)
				c->sig[band] = roam_sta_signal_seen(sta, band,
								   &c->sig_age[band]);
		}

		c->connected = a->connected;
		c->wired = a->wired;
		strncpy(c->enc, a->enc, sizeof(c->enc) - 1);
	}

	node_settle("controller");
}

enum { CL_MAC, CL_BAND, CL_STD, CL_ENC, CL_WIRED, __CL_MAX };

static const struct blobmsg_policy cl_policy[__CL_MAX] = {
	[CL_MAC] = { "mac", BLOBMSG_TYPE_STRING },
	[CL_BAND] = { "band", BLOBMSG_TYPE_STRING },
	[CL_STD] = { "std", BLOBMSG_TYPE_STRING },
	[CL_ENC] = { "encryption", BLOBMSG_TYPE_STRING },
	[CL_WIRED] = { "wired", BLOBMSG_TYPE_INT8 },
};

static void refresh_member(const char *id, uint32_t now)
{
	static const struct blobmsg_policy mp[2] = {
		{ "online", BLOBMSG_TYPE_INT8 },
		{ "clients", BLOBMSG_TYPE_ARRAY },
	};
	static struct blob_buf mb;
	struct blob_attr *tb[2], *cur;
	char path[128], *data;
	int rem;

	snprintf(path, sizeof(path), "%s/%s.json", MEMBERS_DIR, id);
	data = mesh_slurp(path, MESH_REPORT_MAX);
	if (!data)
		return;

	blob_buf_init(&mb, 0);
	if (!blobmsg_add_json_from_string(&mb, data)) {
		free(data);
		return;
	}
	free(data);

	blobmsg_parse(mp, 2, tb, blob_data(mb.head), blob_len(mb.head));
	if (!tb[0] || !blobmsg_get_u8(tb[0]))
		return;

	if (!tb[1]) {
		node_settle(id);
		return;
	}

	blobmsg_for_each_attr(cur, tb[1], rem) {
		struct blob_attr *c[__CL_MAX];
		struct ether_addr *ea;
		struct client_rec *rec;
		uint32_t connected;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE)
			continue;

		blobmsg_parse(cl_policy, __CL_MAX, c, blobmsg_data(cur), blobmsg_data_len(cur));
		if (!c[CL_MAC])
			continue;

		ea = ether_aton(blobmsg_get_string(c[CL_MAC]));
		if (!ea)
			continue;

		connected = num_field(cur, "connected");

		rec = client_upsert(ea->ether_addr_octet, blobmsg_get_string(c[CL_MAC]));
		if (!rec || !client_claimable(rec, connected))
			continue;

		client_touch(rec, id,
			     c[CL_BAND] ? blobmsg_get_string(c[CL_BAND]) : NULL,
			     num_field(cur, "rate"),
			     c[CL_STD] ? blobmsg_get_string(c[CL_STD]) : NULL,
			     num_field(cur, "width"), num_field(cur, "nss"),
			     num_field(cur, "rx_bytes"), num_field(cur, "tx_bytes"), now);
		rec->connected = connected;
		rec->wired = c[CL_WIRED] && blobmsg_get_u8(c[CL_WIRED]);
		rec->enc[0] = '\0';
		if (c[CL_ENC])
			strncpy(rec->enc, blobmsg_get_string(c[CL_ENC]), sizeof(rec->enc) - 1);

		rec->sig[BAND_LOW] = (int)num_field(cur, "signal_24");
		rec->sig[BAND_HIGH] = (int)num_field(cur, "signal_5");
		rec->sig_age[BAND_LOW] = (uint32_t)num_field(cur, "signal_24_age");
		rec->sig_age[BAND_HIGH] = (uint32_t)num_field(cur, "signal_5_age");
	}

	node_settle(id);
}

static void dev_opt(struct uci_session *u, const char *sect, const char *opt, const char *val)
{
	if (val)
		uci_session_set(u, sect, opt, val);
	else
		uci_session_delete(u, sect, opt);
}

static void dev_nodes(struct uci_session *u, const char *sect, const char *csv)
{
	char buf[256], *tok, *save;

	uci_session_delete(u, sect, "node");

	if (!*csv)
		return;

	strncpy(buf, csv, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
		if (*tok)
			uci_session_add_list(u, sect, "node", tok);
}

bool mesh_client_set(const char *mac, const char *band, const char *alias, const char *nodes)
{
	struct uci_session u;
	struct uci_section *found;
	char key[MESH_MAC_MAX];
	size_t i;

	if (!mac || !*mac)
		return false;

	for (i = 0; mac[i] && i < sizeof(key) - 1; i++)
		key[i] = tolower((unsigned char)mac[i]);
	key[i] = '\0';

	if (!uci_session_open(&u, "roamd"))
		return false;

	found = uci_session_find(&u, "device", "mac", key);
	if (!found) {
		found = uci_session_add(&u, "device", NULL);
		if (!found) {
			uci_session_close(&u);
			return false;
		}
		uci_session_set(&u, found->e.name, "mac", key);
	}

	if (band)
		dev_opt(&u, found->e.name, "band", strcmp(band, "both") ? band : NULL);
	if (alias)
		dev_opt(&u, found->e.name, "alias", *alias ? alias : NULL);
	if (nodes)
		dev_nodes(&u, found->e.name, nodes);

	if (!uci_lookup_option(u.ctx, found, "band") && !uci_lookup_option(u.ctx, found, "alias") &&
	    !uci_lookup_option(u.ctx, found, "node"))
		uci_session_delete(&u, found->e.name, NULL);

	uci_session_close(&u);

	roam_config_load();
	mesh_ctrl_sync();

	return true;
}

static bool mac_is_member(const uint8_t *addr)
{
	struct mesh_member *m;

	list_for_each_entry(m, &mesh_members, list) {
		struct ether_addr ea;

		if (ether_aton_r(m->mac, &ea) && !memcmp(ea.ether_addr_octet, addr, 6))
			return true;
	}

	return false;
}

static struct client_rec *client_find(const uint8_t *addr)
{
	unsigned int i;

	for (i = 0; i < MESH_CLIENT_MAX; i++)
		if (clients[i].used && !memcmp(clients[i].addr, addr, 6))
			return &clients[i];

	return NULL;
}

static void refresh_leases(void)
{
	char line[256];
	FILE *f = fopen("/tmp/dhcp.leases", "r");

	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char hw[32], ip[16], host[MESH_NAME_MAX];
		struct ether_addr ea;
		struct client_rec *c;

		if (sscanf(line, "%*s %31s %15s %63s", hw, ip, host) != 3)
			continue;

		if (!ether_aton_r(hw, &ea))
			continue;

		c = client_find(ea.ether_addr_octet);
		if (!c)
			continue;

		strncpy(c->ip, ip, sizeof(c->ip) - 1);
		c->ip[sizeof(c->ip) - 1] = '\0';
		c->host[0] = '\0';
		if (strcmp(host, "*"))
			strncpy(c->host, host, sizeof(c->host) - 1);
	}

	fclose(f);
}

static bool mac_skip(const uint8_t *addr)
{
	uint8_t busy[6];

	if (mac_is_member(addr))
		return true;

	return mesh_ctrl_acquire_mac(busy) && !memcmp(busy, addr, 6);
}

static bool node_known(const char *id)
{
	struct mesh_member *m;

	if (!strcmp(id, "controller"))
		return true;

	list_for_each_entry(m, &mesh_members, list)
		if (!strcmp(m->id, id))
			return true;

	return false;
}

static void clients_refresh(void)
{
	struct mesh_member *m;
	uint32_t now = (uint32_t)time(NULL);
	unsigned int i;

	for (i = 0; i < MESH_CLIENT_MAX; i++) {
		if (clients[i].used && mac_skip(clients[i].addr)) {
			clients[i].used = false;
			clients_dirty = true;
			continue;
		}

		clients[i].state = CLIENT_UNKNOWN;
	}

	list_for_each_entry(m, &mesh_members, list)
		refresh_member(m->id, now);

	refresh_local(now);

	for (i = 0; i < MESH_CLIENT_MAX; i++)
		if (clients[i].used && clients[i].state == CLIENT_UNKNOWN &&
		    !node_known(clients[i].node))
			clients[i].state = CLIENT_OFFLINE;

	if (mesh.role == MESH_CONTROLLER)
		refresh_leases();
}

unsigned int mesh_clients_count(void)
{
	unsigned int i, n = 0;

	clients_refresh();

	for (i = 0; i < MESH_CLIENT_MAX; i++)
		if (clients[i].used && clients[i].state == CLIENT_ONLINE)
			n++;

	return n;
}

unsigned int mesh_clients_local(void)
{
	unsigned int i, n = 0;

	clients_refresh();

	for (i = 0; i < MESH_CLIENT_MAX; i++)
		if (clients[i].used && clients[i].state == CLIENT_ONLINE &&
		    !strcmp(clients[i].node, "controller"))
			n++;

	return n;
}

bool mesh_client_forget(const char *mac)
{
	struct ether_addr *ea;
	unsigned int i;

	if (!mac || !*mac)
		return false;

	ea = ether_aton(mac);
	if (!ea)
		return false;

	for (i = 0; i < MESH_CLIENT_MAX; i++)
		if (clients[i].used &&
		    !memcmp(clients[i].addr, ea->ether_addr_octet, 6)) {
			clients[i].used = false;
			clients_dirty = true;
		}

	mesh_clients_save();

	return mesh_client_set(mac, "both", "", "");
}

#define CLIENTS_DIR	"/tmp/roamd"
#define CLIENTS_FILE	CLIENTS_DIR "/clients.jsonl"

void mesh_clients_save(void)
{
	FILE *f;
	unsigned int i;

	if (!clients_dirty)
		return;

	mkdir(CLIENTS_DIR, 0755);
	f = fopen(CLIENTS_FILE, "w");
	if (!f)
		return;

	for (i = 0; i < MESH_CLIENT_MAX; i++) {
		if (!clients[i].used)
			continue;

		fprintf(f, "{\"mac\":\"%s\",\"node\":\"%s\",\"last_seen\":%u,\"wired\":%u}\n",
			clients[i].mac, clients[i].node, clients[i].last_seen,
			clients[i].wired ? 1 : 0);
	}

	fclose(f);
	clients_dirty = false;
}

void mesh_clients_load(void)
{
	FILE *f = fopen(CLIENTS_FILE, "r");
	char line[256];

	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char mac[18] = "", node[MESH_ID_MAX] = "";
		unsigned int seen = 0, wired = 0;
		struct ether_addr ea;
		struct client_rec *c;

		if (sscanf(line, "{\"mac\":\"%17[^\"]\",\"node\":\"%32[^\"]\",\"last_seen\":%u,\"wired\":%u}",
			   mac, node, &seen, &wired) < 2)
			continue;

		if (!ether_aton_r(mac, &ea))
			continue;

		c = client_upsert(ea.ether_addr_octet, mac);
		if (!c)
			continue;

		strncpy(c->node, node, sizeof(c->node) - 1);
		c->last_seen = seen;
		c->wired = wired != 0;
		c->state = CLIENT_UNKNOWN;
	}

	fclose(f);
	clients_dirty = false;
}

void mesh_clients_dump(struct blob_buf *b)
{
	void *arr;
	unsigned int i;

	clients_refresh();

	arr = blobmsg_open_array(b, "clients");
	for (i = 0; i < MESH_CLIENT_MAX; i++) {
		struct client_rec *c = &clients[i];
		enum roam_lock lock;
		void *e;

		if (!c->used)
			continue;

		e = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "mac", c->mac);
		blobmsg_add_string(b, "node", c->node);
		if (c->wired)
			blobmsg_add_u8(b, "wired", 1);
		if (c->host[0])
			blobmsg_add_string(b, "host", c->host);
		if (c->ip[0])
			blobmsg_add_string(b, "ip", c->ip);
		if (c->band[0])
			blobmsg_add_string(b, "band", c->band);
		if (c->std[0])
			blobmsg_add_string(b, "std", c->std);
		if (c->enc[0])
			blobmsg_add_string(b, "encryption", c->enc);
		if (c->rate)
			blobmsg_add_u32(b, "rate", c->rate);
		if (c->width)
			blobmsg_add_u32(b, "width", c->width);
		if (c->nss)
			blobmsg_add_u32(b, "nss", c->nss);
		if (c->rx_bytes)
			blobmsg_add_u64(b, "rx_bytes", c->rx_bytes);
		if (c->tx_bytes)
			blobmsg_add_u64(b, "tx_bytes", c->tx_bytes);
		lock = roam_device_lock(c->addr);
		if (lock != LOCK_NONE) {
			enum roam_band locked = roam_locked_band(lock);

			blobmsg_add_string(b, "band_lock", roam_band_name(locked));
			blobmsg_add_u8(b, "band_lock_active",
				       mesh_band_usable(roam_band_bit(locked)));
		}

		if (!c->wired && mesh.role == MESH_CONTROLLER &&
		    !client_settled(c) && !mesh_poll_has_place(c->addr))
			blobmsg_add_u8(b, "no_place", 1);

		if (c->sig[BAND_LOW] != ROAMD_NO_SIGNAL) {
			blobmsg_add_u32(b, "signal_24", (uint32_t)c->sig[BAND_LOW]);
			blobmsg_add_u32(b, "signal_24_age", c->sig_age[BAND_LOW]);
		}
		if (c->sig[BAND_HIGH] != ROAMD_NO_SIGNAL) {
			blobmsg_add_u32(b, "signal_5", (uint32_t)c->sig[BAND_HIGH]);
			blobmsg_add_u32(b, "signal_5_age", c->sig_age[BAND_HIGH]);
		}

		blobmsg_add_u32(b, "last_seen", c->last_seen);
		blobmsg_add_string(b, "state", state_name[c->state]);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, arr);
}
