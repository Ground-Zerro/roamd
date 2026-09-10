#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

static struct blob_buf b;

static const char *prefer_name(void)
{
	switch (config.prefer) {
	case PREFER_LOW:
		return "2.4";
	case PREFER_HIGH:
		return "5";
	default:
		return "none";
	}
}

static const char *lock_name(enum roam_lock lock)
{
	switch (lock) {
	case LOCK_LOW:
		return "2.4";
	case LOCK_HIGH:
		return "5";
	default:
		return "both";
	}
}

static void status_add_clients(struct roam_bss *bss)
{
	struct roam_sta *sta;
	void *clients, *entry, *bands;
	enum roam_band band;

	clients = blobmsg_open_table(&b, "clients");

	avl_for_each_element(&roam_sta_tree, sta, avl) {
		if (sta->bss != bss)
			continue;

		entry = blobmsg_open_table(&b, sta->mac);
		blobmsg_add_u32(&b, "signal", sta->band[bss->band].signal);
		blobmsg_add_u32(&b, "connected", (roam_now - sta->connected_since) / 1000);
		blobmsg_add_u8(&b, "btm", sta->btm);
		blobmsg_add_u8(&b, "rrm", sta->rrm);
		blobmsg_add_u32(&b, "steer_count", sta->steer_count);
		blobmsg_add_u8(&b, "btm_rejected", sta->btm_rejected);
		blobmsg_add_string(&b, "band_lock", lock_name(roam_device_lock(sta->addr)));

		bands = blobmsg_open_table(&b, "bands");
		for (band = 0; band < BAND_MAX; band++) {
			void *bi = blobmsg_open_table(&b, roam_band_name(band));

			blobmsg_add_u8(&b, "seen", sta->band[band].present);
			blobmsg_add_u32(&b, "signal", sta->band[band].signal);
			blobmsg_add_u32(&b, "age", sta->band[band].present ?
					(roam_now - sta->band[band].seen) / 1000 : 0);
			blobmsg_close_table(&b, bi);
		}
		blobmsg_close_table(&b, bands);

		blobmsg_close_table(&b, entry);
	}

	blobmsg_close_table(&b, clients);
}

static const char *pair_state(uint32_t issues)
{
	if (issues & PAIR_FATAL)
		return "broken";

	return issues ? "degraded" : "ok";
}

static void status_add_pairing(struct roam_bss *bss)
{
	void *table, *list;
	int i;

	table = blobmsg_open_table(&b, "roaming");
	blobmsg_add_string(&b, "state", pair_state(bss->pair_issues));

	if (bss->pair_peer)
		blobmsg_add_string(&b, "peer", bss->pair_peer->ifname);

	list = blobmsg_open_array(&b, "issues");
	for (i = 0; roam_pair_issues[i]; i++) {
		if (bss->pair_issues & (1u << i))
			blobmsg_add_string(&b, NULL, roam_pair_issues[i]);
	}
	blobmsg_close_array(&b, list);

	blobmsg_close_table(&b, table);
}

static int roamd_status(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct roam_bss *bss;
	void *ifaces, *entry;

	roam_time_update();
	roam_pair_evaluate();

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "enabled", config.enabled);
	blobmsg_add_string(&b, "mesh_role", mesh_role_name(mesh.role));
	blobmsg_add_u8(&b, "band_steering", config.band_steering);
	blobmsg_add_string(&b, "prefer_band", prefer_name());
	blobmsg_add_u8(&b, "fast_transition", config.fast_transition);
	blobmsg_add_u8(&b, "neighbor_reports", config.neighbor_reports);
	blobmsg_add_u8(&b, "bss_transition", config.bss_transition);

	ifaces = blobmsg_open_table(&b, "interfaces");
	list_for_each_entry(bss, &roam_bss_list, list) {
		if (mesh.backhaul_ssid[0] && !strcmp(bss->ssid, mesh.backhaul_ssid))
			continue;

		entry = blobmsg_open_table(&b, bss->ifname);
		blobmsg_add_string(&b, "ssid", bss->ssid);
		blobmsg_add_u32(&b, "freq", bss->freq);
		blobmsg_add_u32(&b, "channel", bss->channel);
		blobmsg_add_string(&b, "band", roam_band_name(bss->band));
		blobmsg_add_u8(&b, "active", bss->active);
		blobmsg_add_u8(&b, "managed", roam_bss_matches(bss));
		blobmsg_add_u8(&b, "neighbor_report", !!bss->nr);
		status_add_pairing(bss);
		status_add_clients(bss);
		blobmsg_close_table(&b, entry);
	}
	blobmsg_close_table(&b, ifaces);

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_reload(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	roam_config_load();
	roam_wireless_apply();
	roam_bss_recheck();
	mesh_start();
	mesh_ctrl_sync();

	return 0;
}

static int roamd_mesh_role(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "role", mesh_role_name(mesh.role));
	blobmsg_add_u8(&b, "enabled", mesh.enabled);
	if (mesh.member_id[0])
		blobmsg_add_string(&b, "member_id", mesh.member_id);

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_status(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	void *members;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "role", mesh_role_name(mesh.role));
	blobmsg_add_u8(&b, "enabled", mesh.enabled);

	if (mesh.role == MESH_NODE) {
		void *ctrl, *counts;

		counts = blobmsg_open_table(&b, "counts");
		blobmsg_add_u32(&b, "controllers", 1);
		blobmsg_add_u32(&b, "nodes", 0);
		blobmsg_add_u32(&b, "clients", mesh_clients_count());
		blobmsg_close_table(&b, counts);

		ctrl = blobmsg_open_table(&b, "controller");
		blobmsg_add_string(&b, "id", mesh.controller_id);
		blobmsg_add_string(&b, "name", mesh.controller_name);
		blobmsg_add_string(&b, "addr", mesh.controller_addr);
		blobmsg_add_u32(&b, "last_contact", mesh_node_contact_age());
		blobmsg_close_table(&b, ctrl);

		mesh_deps_blob(&b);
		blobmsg_add_u8(&b, "backhaul_enabled", mesh.backhaul_enabled);
		blobmsg_add_string(&b, "backhaul_ssid", mesh.backhaul_ssid);
		blobmsg_add_string(&b, "backhaul_key", mesh.backhaul_key);
		blobmsg_add_u8(&b, "wifi_shutdown", mesh.wifi_shutdown);
		blobmsg_add_u8(&b, "auto_update", mesh.auto_update);
		blobmsg_add_string(&b, "member_id", mesh.member_id);
	} else {
		struct mesh_member *m;
		void *counts;
		unsigned int node_count = 0;

		list_for_each_entry(m, &mesh_members, list)
			node_count++;

		counts = blobmsg_open_table(&b, "counts");
		blobmsg_add_u32(&b, "controllers", 1);
		blobmsg_add_u32(&b, "nodes", node_count);
		blobmsg_add_u32(&b, "clients", mesh_clients_count());
		blobmsg_close_table(&b, counts);

		void *self;

		blobmsg_add_string(&b, "controller_id", mesh.controller_id);
		blobmsg_add_string(&b, "controller_name", mesh.controller_name);
		blobmsg_add_u32(&b, "controller_clients", mesh_clients_local());

		self = blobmsg_open_table(&b, "self");
		mesh_self_info(&b);
		blobmsg_close_table(&b, self);
		mesh_deps_blob(&b);
		blobmsg_add_u8(&b, "backhaul_enabled", mesh.backhaul_enabled);
		blobmsg_add_string(&b, "backhaul_ssid", mesh.backhaul_ssid);
		blobmsg_add_string(&b, "backhaul_key", mesh.backhaul_key);
		blobmsg_add_u8(&b, "wifi_shutdown", mesh.wifi_shutdown);
		blobmsg_add_u8(&b, "auto_update", mesh.auto_update);
		blobmsg_add_u32(&b, "auto_update_every", mesh.auto_update_every);
		blobmsg_add_string(&b, "auto_update_unit", mesh.auto_update_unit);
		blobmsg_add_u32(&b, "auto_update_last", mesh.auto_update_last);
		blobmsg_add_string(&b, "auto_update_result", mesh.auto_update_result);
		blobmsg_add_string(&b, "pkg_url", mesh.pkg_url);
		blobmsg_add_string(&b, "pkg_url_default", MESH_PKG_URL_DEFAULT);
		mesh_ctrl_acquire_blob(&b);

		members = blobmsg_open_array(&b, "members");
		list_for_each_entry(m, &mesh_members, list) {
			void *e = blobmsg_open_table(&b, NULL);

			blobmsg_add_string(&b, "id", m->id);
			blobmsg_add_string(&b, "name", m->name);
			blobmsg_add_string(&b, "hostname", m->hostname);
			blobmsg_add_string(&b, "mac", m->mac);
			blobmsg_add_string(&b, "addr", m->addr);
			blobmsg_add_u8(&b, "managed", m->managed);
			mesh_member_live(m->id, &b);
			blobmsg_close_table(&b, e);
		}
		blobmsg_close_array(&b, members);

		mesh_aps_dump(&b, "self_aps");
	}

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum {
	MESH_ARG_ADDR,
	MESH_ARG_TASK,
	MESH_ARG_ID,
	MESH_ARG_MAC,
	MESH_ARG_NAME,
	MESH_ARG_BAND,
	MESH_ARG_ALIAS,
	MESH_ARG_NODES,
	MESH_ARG_NEIGHBORS,
	MESH_ARG_RESET,
	__MESH_ARG_MAX
};

static const struct blobmsg_policy mesh_arg_policy[__MESH_ARG_MAX] = {
	[MESH_ARG_ADDR] = { .name = "addr", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_TASK] = { .name = "task_id", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_ID] = { .name = "id", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_NAME] = { .name = "name", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_BAND] = { .name = "band", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_ALIAS] = { .name = "alias", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_NODES] = { .name = "nodes", .type = BLOBMSG_TYPE_STRING },
	[MESH_ARG_NEIGHBORS] = { .name = "neighbors", .type = BLOBMSG_TYPE_ARRAY },
	[MESH_ARG_RESET] = { .name = "reset", .type = BLOBMSG_TYPE_STRING },
};

static int roamd_mesh_discover(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blob_buf_init(&b, 0);
	mesh_ctrl_discover(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_acquire(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_ADDR])
		return UBUS_STATUS_INVALID_ARGUMENT;

	blob_buf_init(&b, 0);
	mesh_ctrl_acquire(blobmsg_get_string(tb[MESH_ARG_ADDR]), &b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_acquire_status(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_TASK])
		return UBUS_STATUS_INVALID_ARGUMENT;

	blob_buf_init(&b, 0);
	mesh_ctrl_acquire_status(blobmsg_get_string(tb[MESH_ARG_TASK]), &b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_member_remove(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_ID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (tb[MESH_ARG_RESET] && !strcmp(blobmsg_get_string(tb[MESH_ARG_RESET]), "1")) {
		roam_time_update();
		blob_buf_init(&b, 0);
		mesh_ctrl_release(blobmsg_get_string(tb[MESH_ARG_ID]), &b);
		ubus_send_reply(ctx, req, b.head);

		return 0;
	}

	if (!mesh_member_remove(blobmsg_get_string(tb[MESH_ARG_ID])))
		return UBUS_STATUS_NOT_FOUND;

	return 0;
}

enum {
	SET_BH_ENABLED,
	SET_BH_SSID,
	SET_BH_KEY,
	SET_WIFI_SHUTDOWN,
	SET_AUTO_UPDATE,
	SET_AUTO_EVERY,
	SET_AUTO_UNIT,
	SET_PKG_URL,
	SET_NAME,
	__SET_MAX
};

static const struct blobmsg_policy settings_policy[__SET_MAX] = {
	[SET_BH_ENABLED] = { .name = "backhaul_enabled", .type = BLOBMSG_TYPE_STRING },
	[SET_BH_SSID] = { .name = "backhaul_ssid", .type = BLOBMSG_TYPE_STRING },
	[SET_BH_KEY] = { .name = "backhaul_key", .type = BLOBMSG_TYPE_STRING },
	[SET_WIFI_SHUTDOWN] = { .name = "wifi_shutdown", .type = BLOBMSG_TYPE_STRING },
	[SET_AUTO_UPDATE] = { .name = "auto_update", .type = BLOBMSG_TYPE_STRING },
	[SET_AUTO_EVERY] = { .name = "auto_update_every", .type = BLOBMSG_TYPE_STRING },
	[SET_AUTO_UNIT] = { .name = "auto_update_unit", .type = BLOBMSG_TYPE_STRING },
	[SET_PKG_URL] = { .name = "pkg_url", .type = BLOBMSG_TYPE_STRING },
	[SET_NAME] = { .name = "controller_name", .type = BLOBMSG_TYPE_STRING },
};

static int roamd_mesh_settings(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	static const char *const opts[__SET_MAX] = {
		"backhaul_enabled", "backhaul_ssid", "backhaul_key",
		"wifi_shutdown", "auto_update", "auto_update_every",
		"auto_update_unit", "pkg_url", "controller_name"
	};
	struct blob_attr *tb[__SET_MAX];
	struct uci_context *uci;
	struct uci_package *pkg = NULL;
	size_t i;

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(settings_policy, __SET_MAX, tb, blob_data(msg), blob_len(msg));

	uci = uci_alloc_context();
	if (!uci)
		return UBUS_STATUS_UNKNOWN_ERROR;

	if (uci_load(uci, "roamd", &pkg) != UCI_OK) {
		uci_free_context(uci);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	mesh_uci_section(uci, pkg);

	for (i = 0; i < __SET_MAX; i++) {
		struct uci_ptr ptr = {
			.package = "roamd", .section = "mesh", .option = opts[i],
		};

		if (!tb[i])
			continue;
		ptr.value = blobmsg_get_string(tb[i]);
		uci_set(uci, &ptr);
	}

	uci_commit(uci, &pkg, false);
	uci_free_context(uci);

	roam_config_load();
	mesh_ctrl_backhaul_apply();
	mesh_ctrl_autoupdate_arm();

	return 0;
}

static int roamd_mesh_member_update(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_ID] || !tb[MESH_ARG_NAME])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!mesh_member_rename(blobmsg_get_string(tb[MESH_ARG_ID]),
				blobmsg_get_string(tb[MESH_ARG_NAME])))
		return UBUS_STATUS_NOT_FOUND;

	return 0;
}

static int roamd_mesh_update(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_ID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	blob_buf_init(&b, 0);
	mesh_ctrl_update(blobmsg_get_string(tb[MESH_ARG_ID]), &b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_self_update(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blob_buf_init(&b, 0);
	mesh_ctrl_self_update(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_apply(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	if (mesh.role != MESH_NODE)
		return UBUS_STATUS_PERMISSION_DENIED;

	if (!mesh_node_apply(msg))
		return UBUS_STATUS_PERMISSION_DENIED;

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "applied", 1);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_report(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	roam_time_update();

	blob_buf_init(&b, 0);
	mesh_node_report(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_diag(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	roam_time_update();

	blob_buf_init(&b, 0);
	mesh_node_diag(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_steer(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	if (mesh.role != MESH_NODE)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_MAC])
		return UBUS_STATUS_INVALID_ARGUMENT;

	roam_time_update();
	if (!mesh_node_steer(blobmsg_get_string(tb[MESH_ARG_MAC]), tb[MESH_ARG_NEIGHBORS]))
		return UBUS_STATUS_NOT_FOUND;

	return 0;
}

static int roamd_mesh_clients(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	roam_time_update();

	blob_buf_init(&b, 0);
	mesh_clients_dump(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_client_update(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_MAC] ||
	    (!tb[MESH_ARG_BAND] && !tb[MESH_ARG_ALIAS] && !tb[MESH_ARG_NODES]))
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!mesh_client_set(blobmsg_get_string(tb[MESH_ARG_MAC]),
			     tb[MESH_ARG_BAND] ? blobmsg_get_string(tb[MESH_ARG_BAND]) : NULL,
			     tb[MESH_ARG_ALIAS] ? blobmsg_get_string(tb[MESH_ARG_ALIAS]) : NULL,
			     tb[MESH_ARG_NODES] ? blobmsg_get_string(tb[MESH_ARG_NODES]) : NULL))
		return UBUS_STATUS_UNKNOWN_ERROR;

	return 0;
}

static int roamd_mesh_client_forget(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__MESH_ARG_MAX];

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(mesh_arg_policy, __MESH_ARG_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MESH_ARG_MAC])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!mesh_client_forget(blobmsg_get_string(tb[MESH_ARG_MAC])))
		return UBUS_STATUS_UNKNOWN_ERROR;

	return 0;
}

static int roamd_mesh_self_check(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	char *out;
	FILE *f;
	size_t n;

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	f = popen("/usr/libexec/roamd/mesh-selfcheck.sh 2>/dev/null", "r");
	if (!f)
		return UBUS_STATUS_UNKNOWN_ERROR;

	out = calloc(1, 4096);
	if (!out) {
		pclose(f);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	n = fread(out, 1, 4095, f);
	pclose(f);
	out[n] = '\0';

	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_string(&b, out))
		blobmsg_add_string(&b, "error", "check_failed");
	free(out);

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int roamd_mesh_neighbors(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	static const struct blobmsg_policy np = { "neighbors", BLOBMSG_TYPE_ARRAY };
	struct blob_attr *tb = NULL;

	blobmsg_parse(&np, 1, &tb, blob_data(msg), blob_len(msg));
	mesh_neighbors_set(tb);
	roam_neighbors_resync();

	return 0;
}

static int roamd_mesh_log(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	mesh_log_dump(&b, 0);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum {
	INGEST_NODE,
	INGEST_EVENTS,
	__INGEST_MAX
};

static const struct blobmsg_policy ingest_policy[__INGEST_MAX] = {
	[INGEST_NODE] = { .name = "node", .type = BLOBMSG_TYPE_STRING },
	[INGEST_EVENTS] = { .name = "events", .type = BLOBMSG_TYPE_ARRAY },
};

static int roamd_mesh_log_ingest(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__INGEST_MAX];

	if (mesh.role != MESH_CONTROLLER)
		return UBUS_STATUS_PERMISSION_DENIED;

	blobmsg_parse(ingest_policy, __INGEST_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[INGEST_NODE] || !tb[INGEST_EVENTS])
		return UBUS_STATUS_INVALID_ARGUMENT;

	mesh_log_ingest(blobmsg_get_string(tb[INGEST_NODE]), tb[INGEST_EVENTS]);

	return 0;
}

static const struct ubus_method roamd_methods[] = {
	UBUS_METHOD_NOARG("status", roamd_status),
	UBUS_METHOD_NOARG("reload", roamd_reload),
	UBUS_METHOD_NOARG("mesh_status", roamd_mesh_status),
	UBUS_METHOD_NOARG("mesh_clients", roamd_mesh_clients),
	UBUS_METHOD_NOARG("mesh_role", roamd_mesh_role),
	UBUS_METHOD_NOARG("mesh_discover", roamd_mesh_discover),
	UBUS_METHOD("mesh_acquire", roamd_mesh_acquire, mesh_arg_policy),
	UBUS_METHOD("mesh_acquire_status", roamd_mesh_acquire_status, mesh_arg_policy),
	UBUS_METHOD("mesh_member_remove", roamd_mesh_member_remove, mesh_arg_policy),
	UBUS_METHOD("mesh_member_update", roamd_mesh_member_update, mesh_arg_policy),
	UBUS_METHOD("mesh_client_update", roamd_mesh_client_update, mesh_arg_policy),
	UBUS_METHOD("mesh_client_forget", roamd_mesh_client_forget, mesh_arg_policy),
	UBUS_METHOD_NOARG("mesh_self_check", roamd_mesh_self_check),
	UBUS_METHOD("mesh_update", roamd_mesh_update, mesh_arg_policy),
	UBUS_METHOD_NOARG("mesh_self_update", roamd_mesh_self_update),
	UBUS_METHOD("mesh_settings", roamd_mesh_settings, settings_policy),
	UBUS_METHOD_NOARG("mesh_apply", roamd_mesh_apply),
	UBUS_METHOD_NOARG("mesh_report", roamd_mesh_report),
	UBUS_METHOD_NOARG("mesh_diag", roamd_mesh_diag),
	UBUS_METHOD("mesh_steer", roamd_mesh_steer, mesh_arg_policy),
	UBUS_METHOD_NOARG("mesh_neighbors", roamd_mesh_neighbors),
	UBUS_METHOD_NOARG("mesh_log", roamd_mesh_log),
	UBUS_METHOD("mesh_log_ingest", roamd_mesh_log_ingest, ingest_policy),
};

static struct ubus_object_type roamd_type =
	UBUS_OBJECT_TYPE("roamd", roamd_methods);

static struct ubus_object roamd_object = {
	.name = "roamd",
	.type = &roamd_type,
	.methods = roamd_methods,
	.n_methods = ARRAY_SIZE(roamd_methods),
};

void roam_ubus_object_init(void)
{
	ubus_add_object(ubus_ctx, &roamd_object);
}
