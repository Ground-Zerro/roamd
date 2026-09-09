#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/ether.h>

#include <libubox/avl-cmp.h>

#include "roamd.h"
#include "mesh.h"

#define HOSTAPD_PREFIX		"hostapd."
#define HOSTAPD_PREFIX_LEN	(sizeof(HOSTAPD_PREFIX) - 1)
#define UBUS_TIMEOUT		500

struct avl_tree roam_bss_tree;
struct list_head roam_bss_list;

#define PROVISION_DELAY		2000

static struct blob_buf b;
static struct ubus_event_handler add_handler;
static struct uloop_timeout provision_timer;

static const char *const event_names[EVENT_MAX] = { "probe", "auth", "assoc" };

static void provision_run(struct uloop_timeout *t)
{
	roam_wireless_apply();
}

const char *roam_band_name(enum roam_band band)
{
	return band == BAND_LOW ? "2.4" : "5";
}

static enum roam_band band_from_freq(int freq)
{
	return freq < 3000 ? BAND_LOW : BAND_HIGH;
}

int roam_bss_invoke(struct roam_bss *bss, const char *method, struct blob_buf *buf)
{
	int ret = ubus_invoke(ubus_ctx, bss->obj_id, method, buf ? buf->head : NULL,
			      NULL, NULL, UBUS_TIMEOUT);

	if (ret)
		roam_log(ROAM_L_ERR, "roamd: %s on %s failed: %s",
			 method, bss->ifname, ubus_strerror(ret));

	return ret;
}

bool roam_bss_matches(const struct roam_bss *bss)
{
	if (!bss->active || !bss->ssid[0])
		return false;

	if (mesh.backhaul_ssid[0] && !strcmp(bss->ssid, mesh.backhaul_ssid))
		return false;

	return !config.ssid[0] || !strcmp(config.ssid, bss->ssid);
}

struct roam_bss *roam_bss_by_bssid(const uint8_t *bssid)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list) {
		if (!memcmp(bss->bssid, bssid, 6))
			return bss;
	}

	return NULL;
}

struct roam_bss *roam_bss_target(const struct roam_bss *from)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list) {
		if (bss == from || !roam_bss_matches(bss))
			continue;
		if (bss->band == from->band || strcmp(bss->ssid, from->ssid))
			continue;

		return bss;
	}

	return NULL;
}

static void status_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	enum {
		ST_STATUS,
		ST_BSSID,
		ST_SSID,
		ST_FREQ,
		ST_CHANNEL,
		ST_OP_CLASS,
		__ST_MAX
	};
	static const struct blobmsg_policy policy[__ST_MAX] = {
		[ST_STATUS] = { "status", BLOBMSG_TYPE_STRING },
		[ST_BSSID] = { "bssid", BLOBMSG_TYPE_STRING },
		[ST_SSID] = { "ssid", BLOBMSG_TYPE_STRING },
		[ST_FREQ] = { "freq", BLOBMSG_TYPE_INT32 },
		[ST_CHANNEL] = { "channel", BLOBMSG_TYPE_INT32 },
		[ST_OP_CLASS] = { "op_class", BLOBMSG_TYPE_INT32 },
	};
	struct blob_attr *tb[__ST_MAX];
	struct roam_bss *bss = req->priv;
	struct ether_addr *ea;

	blobmsg_parse(policy, __ST_MAX, tb, blob_data(msg), blob_len(msg));

	bss->active = tb[ST_STATUS] && !strcmp(blobmsg_get_string(tb[ST_STATUS]), "ENABLED");

	if (tb[ST_SSID])
		snprintf(bss->ssid, sizeof(bss->ssid), "%s", blobmsg_get_string(tb[ST_SSID]));

	if (tb[ST_BSSID]) {
		ea = ether_aton(blobmsg_get_string(tb[ST_BSSID]));
		if (ea)
			memcpy(bss->bssid, ea->ether_addr_octet, 6);
	}

	if (tb[ST_FREQ]) {
		bss->freq = blobmsg_get_u32(tb[ST_FREQ]);
		bss->band = band_from_freq(bss->freq);
	}

	if (tb[ST_CHANNEL])
		bss->channel = blobmsg_get_u32(tb[ST_CHANNEL]);

	if (tb[ST_OP_CLASS])
		bss->op_class = blobmsg_get_u32(tb[ST_OP_CLASS]);
}

static bool client_ext_capa_btm(struct blob_attr *client)
{
	static const struct blobmsg_policy policy = { "extended_capabilities", BLOBMSG_TYPE_ARRAY };
	struct blob_attr *list = NULL, *cur;
	int rem, i = 0;

	blobmsg_parse(&policy, 1, &list, blobmsg_data(client), blobmsg_data_len(client));
	if (!list)
		return false;

	blobmsg_for_each_attr(cur, list, rem) {
		if (i++ != 2)
			continue;
		if (blobmsg_type(cur) != BLOBMSG_TYPE_INT32)
			return false;

		return !!(blobmsg_get_u32(cur) & (1 << 3));
	}

	return false;
}

static bool client_rrm_beacon(struct blob_attr *client)
{
	static const struct blobmsg_policy policy = { "rrm", BLOBMSG_TYPE_ARRAY };
	struct blob_attr *list = NULL;

	blobmsg_parse(&policy, 1, &list, blobmsg_data(client), blobmsg_data_len(client));
	if (!list || !blobmsg_data_len(list))
		return false;

	return !!(blobmsg_get_u32(blobmsg_data(list)) & 0x70);
}

static void client_update(struct roam_bss *bss, struct blob_attr *client, const char *mac)
{
	enum {
		CL_ASSOC,
		CL_HT,
		CL_SIGNAL,
		__CL_MAX
	};
	static const struct blobmsg_policy policy[__CL_MAX] = {
		[CL_ASSOC] = { "assoc", BLOBMSG_TYPE_BOOL },
		[CL_HT] = { "ht", BLOBMSG_TYPE_BOOL },
		[CL_SIGNAL] = { "signal", BLOBMSG_TYPE_INT32 },
	};
	struct blob_attr *tb[__CL_MAX];
	struct ether_addr *ea = ether_aton(mac);
	struct roam_sta *sta;
	int signal = ROAMD_NO_SIGNAL;

	if (!ea)
		return;

	blobmsg_parse(policy, __CL_MAX, tb, blobmsg_data(client), blobmsg_data_len(client));

	if (!tb[CL_ASSOC] || !blobmsg_get_u8(tb[CL_ASSOC]))
		return;

	sta = roam_sta_get((uint8_t *)ea->ether_addr_octet, true);
	if (!sta)
		return;

	if (tb[CL_SIGNAL])
		signal = (int32_t)blobmsg_get_u32(tb[CL_SIGNAL]);

	sta->btm = client_ext_capa_btm(client);
	sta->rrm = client_rrm_beacon(client);
	sta->band[bss->band].ht = tb[CL_HT] && blobmsg_get_u8(tb[CL_HT]);

	roam_sta_set_connected(sta, bss, signal);
}

static void clients_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const struct blobmsg_policy policy = { "clients", BLOBMSG_TYPE_TABLE };
	struct roam_bss *bss = req->priv;
	struct blob_attr *list = NULL, *cur;
	int rem;

	blobmsg_parse(&policy, 1, &list, blob_data(msg), blob_len(msg));
	if (!list)
		return;

	blobmsg_for_each_attr(cur, list, rem)
		client_update(bss, cur, blobmsg_name(cur));
}

static void nr_own_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const struct blobmsg_policy policy = { "value", BLOBMSG_TYPE_ARRAY };
	struct roam_bss *bss = req->priv;
	struct blob_attr *nr = NULL;

	blobmsg_parse(&policy, 1, &nr, blob_data(msg), blob_len(msg));
	if (!nr)
		return;

	free(bss->nr);
	bss->nr = blob_memdup(nr);
}

static void neighbor_sync(struct roam_bss *bss)
{
	struct roam_bss *peer;
	void *list;
	int count = 0;

	if (!config.neighbor_reports)
		return;

	blob_buf_init(&b, 0);
	list = blobmsg_open_array(&b, "list");

	list_for_each_entry(peer, &roam_bss_list, list) {
		if (peer == bss || !peer->nr || !roam_bss_matches(peer))
			continue;
		if (strcmp(peer->ssid, bss->ssid))
			continue;
		if (count >= ROAMD_MAX_NEIGHBORS)
			break;

		blobmsg_add_field(&b, BLOBMSG_TYPE_ARRAY, "",
				  blobmsg_data(peer->nr), blobmsg_data_len(peer->nr));
		count++;
	}

	mesh_neighbors_append(&b, bss->ssid, &count, ROAMD_MAX_NEIGHBORS);

	blobmsg_close_array(&b, list);

	if (count)
		roam_bss_invoke(bss, "rrm_nr_set", &b);
}

void roam_neighbors_resync(void)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list)
		if (roam_bss_matches(bss))
			neighbor_sync(bss);
}

static void bss_poll(struct uloop_timeout *t)
{
	struct roam_bss *bss = container_of(t, struct roam_bss, poll);

	roam_time_update();

	ubus_invoke(ubus_ctx, bss->obj_id, "get_status", NULL, status_cb, bss, UBUS_TIMEOUT);

	if (roam_bss_matches(bss)) {
		ubus_invoke(ubus_ctx, bss->obj_id, "get_clients", NULL, clients_cb, bss, UBUS_TIMEOUT);

		if (!bss->nr)
			ubus_invoke(ubus_ctx, bss->obj_id, "rrm_nr_get_own", NULL, nr_own_cb, bss, UBUS_TIMEOUT);

		neighbor_sync(bss);
		roam_policy_run(bss);
	}

	roam_sta_expire();
	uloop_timeout_set(t, config.poll_interval);
}

static int handle_sta_event(struct roam_bss *bss, const char *method, struct blob_attr *msg)
{
	enum {
		EV_ADDR,
		EV_SIGNAL,
		EV_FREQ,
		__EV_MAX
	};
	static const struct blobmsg_policy policy[__EV_MAX] = {
		[EV_ADDR] = { "address", BLOBMSG_TYPE_STRING },
		[EV_SIGNAL] = { "signal", BLOBMSG_TYPE_INT32 },
		[EV_FREQ] = { "freq", BLOBMSG_TYPE_INT32 },
	};
	struct blob_attr *tb[__EV_MAX];
	struct ether_addr *ea;
	struct roam_sta *sta;
	enum roam_event ev;
	int signal = ROAMD_NO_SIGNAL;

	for (ev = 0; ev < EVENT_MAX; ev++) {
		if (!strcmp(method, event_names[ev]))
			break;
	}

	if (ev == EVENT_MAX)
		return 0;

	blobmsg_parse(policy, __EV_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[EV_ADDR])
		return 0;

	ea = ether_aton(blobmsg_get_string(tb[EV_ADDR]));
	if (!ea)
		return 0;

	if (tb[EV_SIGNAL])
		signal = (int32_t)blobmsg_get_u32(tb[EV_SIGNAL]);

	roam_time_update();
	roam_sta_event(bss, (uint8_t *)ea->ether_addr_octet, signal);

	sta = roam_sta_get((uint8_t *)ea->ether_addr_octet, false);
	if (!sta || roam_policy_allow(sta, bss, ev))
		return 0;

	roam_log(ROAM_L_DEBUG, "roamd: %s %s on %s GHz (%s) rejected",
		 method, sta->mac, roam_band_name(bss->band), bss->ifname);

	return 17;
}

static int handle_btm_response(struct roam_bss *bss, struct blob_attr *msg)
{
	enum {
		BR_ADDR,
		BR_STATUS,
		__BR_MAX
	};
	static const struct blobmsg_policy policy[__BR_MAX] = {
		[BR_ADDR] = { "address", BLOBMSG_TYPE_STRING },
		[BR_STATUS] = { "status-code", BLOBMSG_TYPE_INT8 },
	};
	struct blob_attr *tb[__BR_MAX];
	struct ether_addr *ea;
	struct roam_sta *sta;

	blobmsg_parse(policy, __BR_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[BR_ADDR] || !tb[BR_STATUS])
		return 0;

	ea = ether_aton(blobmsg_get_string(tb[BR_ADDR]));
	if (!ea)
		return 0;

	sta = roam_sta_get((uint8_t *)ea->ether_addr_octet, false);
	if (!sta)
		return 0;

	roam_policy_btm_response(sta, blobmsg_get_u8(tb[BR_STATUS]));

	return 0;
}

static int handle_beacon_report(struct blob_attr *msg)
{
	enum {
		BR_ADDR,
		BR_BSSID,
		BR_RCPI,
		__BR_MAX
	};
	static const struct blobmsg_policy policy[__BR_MAX] = {
		[BR_ADDR] = { "address", BLOBMSG_TYPE_STRING },
		[BR_BSSID] = { "bssid", BLOBMSG_TYPE_STRING },
		[BR_RCPI] = { "rcpi", BLOBMSG_TYPE_INT16 },
	};
	struct blob_attr *tb[__BR_MAX];
	struct ether_addr *ea, *bssid;
	struct roam_bss *target;
	struct roam_sta *sta;
	unsigned rcpi;
	int signal;

	blobmsg_parse(policy, __BR_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[BR_ADDR] || !tb[BR_BSSID] || !tb[BR_RCPI])
		return 0;

	ea = ether_aton(blobmsg_get_string(tb[BR_ADDR]));
	if (!ea)
		return 0;

	sta = roam_sta_get((uint8_t *)ea->ether_addr_octet, false);
	if (!sta)
		return 0;

	bssid = ether_aton(blobmsg_get_string(tb[BR_BSSID]));
	if (!bssid)
		return 0;

	target = roam_bss_by_bssid((uint8_t *)bssid->ether_addr_octet);
	if (!target)
		return 0;

	rcpi = (uint8_t)blobmsg_get_u16(tb[BR_RCPI]);
	if (rcpi >= ROAMD_RCPI_IMPLAUSIBLE) {
		roam_log(ROAM_L_DEBUG, "roamd: beacon report %s on %s GHz dropped, rcpi %u",
			 sta->mac, roam_band_name(target->band), rcpi);
		return 0;
	}

	signal = ((int)rcpi / 2) - 110;

	sta->beacon_req_silent = 0;
	sta->band[target->band].signal = signal;
	sta->band[target->band].seen = roam_now;
	sta->band[target->band].present = true;

	roam_log(ROAM_L_DEBUG, "roamd: beacon report %s on %s GHz signal %d (rcpi %u)",
		 sta->mac, roam_band_name(target->band), signal, rcpi);

	return 0;
}

static int handle_disassoc(struct blob_attr *msg)
{
	static const struct blobmsg_policy policy = { "address", BLOBMSG_TYPE_STRING };
	struct blob_attr *tb = NULL;
	struct ether_addr *ea;
	struct roam_sta *sta;

	blobmsg_parse(&policy, 1, &tb, blob_data(msg), blob_len(msg));
	if (!tb)
		return 0;

	ea = ether_aton(blobmsg_get_string(tb));
	if (!ea)
		return 0;

	sta = roam_sta_get((uint8_t *)ea->ether_addr_octet, false);
	if (sta)
		roam_sta_disconnected(sta);

	return 0;
}

static int bss_notify_cb(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	struct roam_bss *bss = container_of(obj, struct roam_bss, sub.obj);

	if (!config.enabled)
		return 0;

	if (!strcmp(method, "bss-transition-response"))
		return handle_btm_response(bss, msg);

	if (!strcmp(method, "beacon-report"))
		return handle_beacon_report(msg);

	if (!strcmp(method, "disassoc") || !strcmp(method, "deauth"))
		return handle_disassoc(msg);

	return handle_sta_event(bss, method, msg);
}

static void bss_free(struct roam_bss *bss)
{
	uloop_timeout_cancel(&bss->poll);

	if (bss->subscribed) {
		bss->subscribed = false;
		ubus_unregister_subscriber(ubus_ctx, &bss->sub);
	}

	roam_sta_drop_bss(bss);
	list_del(&bss->list);
	avl_delete(&roam_bss_tree, &bss->avl);
	free(bss->nr);
	free((void *)bss->avl.key);
	free(bss);
}

static void bss_remove_cb(struct ubus_context *ctx, struct ubus_subscriber *sub, uint32_t id)
{
	struct roam_bss *bss = container_of(sub, struct roam_bss, sub);

	roam_log(ROAM_L_INFO, "roamd: interface %s removed", bss->ifname);
	bss_free(bss);
}

static void bss_enable_management(struct roam_bss *bss)
{
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "notify_response", 1);
	roam_bss_invoke(bss, "notify_response", &b);

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "neighbor_report", config.neighbor_reports);
	blobmsg_add_u8(&b, "beacon_report", config.neighbor_reports);
	blobmsg_add_u8(&b, "bss_transition", config.bss_transition);
	roam_bss_invoke(bss, "bss_mgmt_enable", &b);
}

static void bss_register(const char *path, uint32_t id)
{
	struct roam_bss *bss;
	const char *ifname;

	if (strncmp(path, HOSTAPD_PREFIX, HOSTAPD_PREFIX_LEN))
		return;

	ifname = path + HOSTAPD_PREFIX_LEN;
	if (!*ifname)
		return;

	bss = avl_find_element(&roam_bss_tree, path, bss, avl);
	if (bss) {
		bss->obj_id = id;
		bss_enable_management(bss);
		return;
	}

	bss = calloc(1, sizeof(*bss));
	if (!bss)
		return;

	bss->avl.key = strdup(path);
	if (!bss->avl.key) {
		free(bss);
		return;
	}

	bss->obj_id = id;
	bss->poll.cb = bss_poll;
	bss->sub.cb = bss_notify_cb;
	bss->sub.remove_cb = bss_remove_cb;
	snprintf(bss->ifname, sizeof(bss->ifname), "%s", ifname);

	if (avl_insert(&roam_bss_tree, &bss->avl)) {
		free((void *)bss->avl.key);
		free(bss);
		return;
	}

	list_add_tail(&bss->list, &roam_bss_list);

	if (ubus_register_subscriber(ubus_ctx, &bss->sub) ||
	    ubus_subscribe(ubus_ctx, &bss->sub, id))
		roam_log(ROAM_L_ERR, "roamd: cannot subscribe to %s", path);
	else
		bss->subscribed = true;

	bss_enable_management(bss);

	roam_log(ROAM_L_INFO, "roamd: tracking interface %s", bss->ifname);
	uloop_timeout_set(&bss->poll, 100);
	uloop_timeout_set(&provision_timer, PROVISION_DELAY);
}

static void object_add_cb(struct ubus_context *ctx, struct ubus_event_handler *ev,
			  const char *type, struct blob_attr *msg)
{
	enum {
		OA_ID,
		OA_PATH,
		__OA_MAX
	};
	static const struct blobmsg_policy policy[__OA_MAX] = {
		[OA_ID] = { "id", BLOBMSG_TYPE_INT32 },
		[OA_PATH] = { "path", BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *tb[__OA_MAX];

	blobmsg_parse(policy, __OA_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[OA_ID] || !tb[OA_PATH])
		return;

	bss_register(blobmsg_get_string(tb[OA_PATH]), blobmsg_get_u32(tb[OA_ID]));
}

static void lookup_cb(struct ubus_context *ctx, struct ubus_object_data *obj, void *priv)
{
	bss_register(obj->path, obj->id);
}

void roam_bss_recheck(void)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list) {
		bss_enable_management(bss);
		uloop_timeout_set(&bss->poll, 100);
	}
}

void roam_bss_setup(void)
{
	avl_init(&roam_bss_tree, avl_strcmp, false, NULL);
	INIT_LIST_HEAD(&roam_bss_list);
	provision_timer.cb = provision_run;
}

void roam_bss_init(void)
{
	roam_bss_free_all();

	add_handler.cb = object_add_cb;
	ubus_register_event_handler(ubus_ctx, &add_handler, "ubus.object.add");
	ubus_lookup(ubus_ctx, HOSTAPD_PREFIX "*", lookup_cb, NULL);
}

void roam_bss_free_all(void)
{
	struct roam_bss *bss, *tmp;

	uloop_timeout_cancel(&provision_timer);

	list_for_each_entry_safe(bss, tmp, &roam_bss_list, list)
		bss_free(bss);
}
