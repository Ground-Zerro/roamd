#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
#define NEIGHBOR_REFRESH	60000
#define HOSTAPD_CTRL_DIR	"/var/run/hostapd"
#define HOSTAPD_CTRL_TIMEOUT	1000
#define RSN_OUI			0x000fac

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

uint8_t roam_band_bit(enum roam_band band)
{
	return band == BAND_LOW ? MESH_BAND_24 : MESH_BAND_5;
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
	const struct mesh_network *net;

	if (!bss->active || !bss->ssid[0])
		return false;

	if (mesh.backhaul_ssid[0] && !strcmp(bss->ssid, mesh.backhaul_ssid))
		return false;

	net = mesh_network_by_ssid(bss->ssid);

	return !net || net->roaming;
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

static void client_update(struct roam_bss *bss, struct blob_attr *client, const char *mac,
			  uint32_t round)
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

	sta->polled = round;
	sta->btm = client_ext_capa_btm(client);
	sta->rrm = client_rrm_beacon(client);
	sta->band[bss->band].ht = tb[CL_HT] && blobmsg_get_u8(tb[CL_HT]);

	roam_sta_set_connected(sta, bss, signal);
}

static void clients_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const struct blobmsg_policy policy = { "clients", BLOBMSG_TYPE_TABLE };
	static uint32_t round;
	struct roam_bss *bss = req->priv;
	struct blob_attr *list = NULL, *cur;
	struct roam_sta *sta;
	int rem;

	blobmsg_parse(&policy, 1, &list, blob_data(msg), blob_len(msg));
	if (!list)
		return;

	round++;
	blobmsg_for_each_attr(cur, list, rem)
		client_update(bss, cur, blobmsg_name(cur), round);

	avl_for_each_element(&roam_sta_tree, sta, avl) {
		if (sta->bss == bss && sta->polled != round)
			roam_sta_disconnected(sta);
	}
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

static uint32_t blob_fingerprint(const struct blob_attr *attr)
{
	const uint8_t *p = (const uint8_t *)attr;
	size_t len = blob_raw_len(attr);
	uint32_t h = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++)
		h = (h ^ p[i]) * 16777619u;

	return h;
}

static void neighbor_sync(struct roam_bss *bss)
{
	struct roam_bss *peer;
	uint32_t fp;
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

	if (!count && !bss->nr_sent)
		return;

	fp = blob_fingerprint(b.head);
	if (fp == bss->nr_sent && roam_now - bss->nr_sent_at < NEIGHBOR_REFRESH)
		return;

	roam_bss_invoke(bss, "rrm_nr_set", &b);
	bss->nr_sent = fp;
	bss->nr_sent_at = roam_now;
}

void roam_neighbors_resync(void)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list)
		if (roam_bss_matches(bss))
			neighbor_sync(bss);
}

static const char *akm_security(int wpa, unsigned int oui, unsigned int suite)
{
	if (wpa == 1)
		return "WPA";

	if (oui != RSN_OUI)
		return NULL;

	switch (suite) {
	case 1: case 2: case 3: case 4: case 6:
		return "WPA2";
	case 5: case 8: case 9: case 11: case 12: case 13: case 24: case 25:
		return "WPA3";
	case 18:
		return "OWE";
	}

	return NULL;
}

struct sta_query {
	struct uloop_fd fd;
	struct uloop_timeout timeout;
	struct sockaddr_un local;
	uint8_t addr[6];
	uint64_t since;
};

static void sta_query_free(struct sta_query *q)
{
	uloop_fd_delete(&q->fd);
	uloop_timeout_cancel(&q->timeout);
	close(q->fd.fd);
	unlink(q->local.sun_path);
	free(q);
}

static void sta_query_expired(struct uloop_timeout *t)
{
	sta_query_free(container_of(t, struct sta_query, timeout));
}

static const char *sta_reply_security(const char *reply)
{
	unsigned int o1, o2, o3, suite;
	const char *wpa = strstr(reply, "\nwpa=");
	const char *akm = strstr(reply, "\nAKMSuiteSelector=");

	if (!wpa || !akm ||
	    sscanf(akm, "\nAKMSuiteSelector=%x-%x-%x-%u", &o1, &o2, &o3, &suite) != 4)
		return NULL;

	return akm_security(atoi(wpa + 5), (o1 << 16) | (o2 << 8) | o3, suite);
}

static void sta_query_reply(struct uloop_fd *fd, unsigned int events)
{
	struct sta_query *q = container_of(fd, struct sta_query, fd);
	char reply[4096];
	ssize_t got = recv(fd->fd, reply, sizeof(reply) - 1, 0);
	struct roam_sta *sta;

	if (got > 0) {
		reply[got] = 0;
		sta = roam_sta_get(q->addr, false);
		if (sta && sta->security_since == q->since)
			sta->security = sta_reply_security(reply);
	}

	sta_query_free(q);
}

void roam_bss_sta_security_query(const struct roam_bss *bss, const uint8_t *addr, uint64_t since)
{
	static unsigned int seq;
	struct sockaddr_un dest = { .sun_family = AF_UNIX };
	struct sta_query *q;
	struct stat dir;
	char cmd[32];
	int len;

	if (stat(HOSTAPD_CTRL_DIR, &dir))
		return;

	q = calloc(1, sizeof(*q));
	if (!q)
		return;

	q->fd.fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (q->fd.fd < 0) {
		free(q);
		return;
	}

	q->fd.cb = sta_query_reply;
	q->timeout.cb = sta_query_expired;
	q->local.sun_family = AF_UNIX;
	memcpy(q->addr, addr, 6);
	q->since = since;

	snprintf(q->local.sun_path, sizeof(q->local.sun_path), "/var/run/roamd-hostapd-%d-%u",
		 (int)getpid(), seq++);
	snprintf(dest.sun_path, sizeof(dest.sun_path), HOSTAPD_CTRL_DIR "/%s", bss->ifname);
	len = snprintf(cmd, sizeof(cmd), "STA %02x:%02x:%02x:%02x:%02x:%02x",
		       addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	unlink(q->local.sun_path);

	if (bind(q->fd.fd, (struct sockaddr *)&q->local, sizeof(q->local)) ||
	    chown(q->local.sun_path, dir.st_uid, dir.st_gid) ||
	    connect(q->fd.fd, (struct sockaddr *)&dest, sizeof(dest)) ||
	    send(q->fd.fd, cmd, len, 0) != len) {
		close(q->fd.fd);
		unlink(q->local.sun_path);
		free(q);
		return;
	}

	uloop_fd_add(&q->fd, ULOOP_READ);
	uloop_timeout_set(&q->timeout, HOSTAPD_CTRL_TIMEOUT);
}

static void bss_enable_management(struct roam_bss *bss);

static const struct {
	const char *method;
	ubus_data_handler_t data_cb;
} bss_queries[__BSS_QUERY_MAX] = {
	[BSS_QUERY_STATUS] = { "get_status", status_cb },
	[BSS_QUERY_CLIENTS] = { "get_clients", clients_cb },
	[BSS_QUERY_NR] = { "rrm_nr_get_own", nr_own_cb },
};

static void bss_poll_finish(struct roam_bss *bss)
{
	if (roam_bss_matches(bss)) {
		neighbor_sync(bss);
		roam_policy_run(bss);
	}

	roam_sta_expire();
	uloop_timeout_set(&bss->poll, config.poll_interval);
}

static void query_complete_cb(struct ubus_request *req, int ret)
{
	struct roam_bss *bss = req->priv;

	if (ret)
		roam_log(ROAM_L_DEBUG, "roamd: %s on %s failed: %s",
			 bss_queries[req - bss->query].method, bss->ifname, ubus_strerror(ret));

	bss->query_pending &= ~(1u << (req - bss->query));

	if (!bss->query_pending)
		bss_poll_finish(bss);
}

static void query_start(struct roam_bss *bss, enum roam_bss_query q)
{
	struct ubus_request *req = &bss->query[q];

	if (ubus_invoke_async(ubus_ctx, bss->obj_id, bss_queries[q].method, NULL, req))
		return;

	req->data_cb = bss_queries[q].data_cb;
	req->complete_cb = query_complete_cb;
	req->priv = bss;
	bss->query_pending |= 1u << q;
	ubus_complete_request_async(ubus_ctx, req);
}

static void queries_abort(struct roam_bss *bss)
{
	unsigned int q;

	for (q = 0; q < __BSS_QUERY_MAX; q++)
		if (bss->query_pending & (1u << q))
			ubus_abort_request(ubus_ctx, &bss->query[q]);

	bss->query_pending = 0;
}

static void bss_poll(struct uloop_timeout *t)
{
	struct roam_bss *bss = container_of(t, struct roam_bss, poll);

	roam_time_update();

	if (bss->query_pending) {
		roam_log(ROAM_L_ERR, "roamd: hostapd on %s did not answer in %d ms",
			 bss->ifname, UBUS_TIMEOUT);
		queries_abort(bss);
		bss_poll_finish(bss);
		return;
	}

	if (bss->mgmt_pending) {
		bss_enable_management(bss);
		if (!bss->mgmt_pending)
			roam_log(ROAM_L_INFO, "roamd: management enabled on %s after retry", bss->ifname);
	}

	query_start(bss, BSS_QUERY_STATUS);

	if (roam_bss_matches(bss)) {
		query_start(bss, BSS_QUERY_CLIENTS);

		if (!bss->nr)
			query_start(bss, BSS_QUERY_NR);
	}

	if (bss->query_pending)
		uloop_timeout_set(&bss->poll, UBUS_TIMEOUT);
	else
		bss_poll_finish(bss);
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
	if (!target && !mesh_neighbor_known((uint8_t *)bssid->ether_addr_octet))
		return 0;

	rcpi = (uint8_t)blobmsg_get_u16(tb[BR_RCPI]);
	if (rcpi >= ROAMD_RCPI_IMPLAUSIBLE) {
		roam_log(ROAM_L_DEBUG, "roamd: beacon report %s for %s dropped, rcpi %u",
			 sta->mac, blobmsg_get_string(tb[BR_BSSID]), rcpi);
		return 0;
	}

	signal = ((int)rcpi / 2) - 110;

	sta->beacon_req_silent = 0;
	roam_sta_measured(sta, (uint8_t *)bssid->ether_addr_octet, signal);

	if (!target || (sta->bss && target->band == sta->bss->band)) {
		roam_log(ROAM_L_DEBUG, "roamd: beacon report %s for %s signal %d (rcpi %u)",
			 sta->mac, blobmsg_get_string(tb[BR_BSSID]), signal, rcpi);
		return 0;
	}

	sta->band[target->band].signal = signal;
	sta->band[target->band].seen = roam_now;
	sta->band[target->band].present = true;

	roam_log(ROAM_L_DEBUG, "roamd: beacon report %s on %s GHz signal %d (rcpi %u)",
		 sta->mac, roam_band_name(target->band), signal, rcpi);

	return 0;
}

static int handle_disassoc(struct roam_bss *bss, struct blob_attr *msg)
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
	if (sta && sta->bss == bss)
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

	if (!strcmp(method, "beacon-report"))
		return handle_beacon_report(msg);

	if (!strcmp(method, "disassoc") || !strcmp(method, "deauth"))
		return handle_disassoc(bss, msg);

	return handle_sta_event(bss, method, msg);
}

static void bss_free(struct roam_bss *bss)
{
	uloop_timeout_cancel(&bss->poll);
	queries_abort(bss);

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
	int notify, mgmt;

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "notify_response", 1);
	notify = roam_bss_invoke(bss, "notify_response", &b);

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "neighbor_report", config.neighbor_reports);
	blobmsg_add_u8(&b, "beacon_report", config.neighbor_reports);
	blobmsg_add_u8(&b, "bss_transition", config.bss_transition);
	mgmt = roam_bss_invoke(bss, "bss_mgmt_enable", &b);

	bss->mgmt_pending = notify == UBUS_STATUS_TIMEOUT || mgmt == UBUS_STATUS_TIMEOUT;
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
		if (!bss->query_pending)
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
