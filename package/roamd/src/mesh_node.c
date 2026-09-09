#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <netinet/ether.h>

#include <libubox/uloop.h>

#include "roamd.h"
#include "mesh.h"

#define MESH_CONTACT_TIMEOUT	60000
#define MESH_WATCH_INTERVAL	10000
#define MESH_BH_STA		"mesh_bh_sta"
#define MESH_BH_BAND_TIMEOUT	45000

static struct uloop_timeout watch_timer;
static uint64_t last_contact;
static bool contact_seen;
static uint64_t bh_since;
static bool aps_down;
static bool aps_state_known;
static bool bh_up;
static int bh_band;

void mesh_node_touch(void)
{
	last_contact = roam_now;
	contact_seen = true;
}

static void aps_set_disabled(bool disabled)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	uint32_t id;
	bool changed = false;

	if (!ctx)
		return;

	if (uci_load(ctx, "wireless", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return;
	}

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *mode = uci_lookup_option_string(ctx, s, "mode");
		struct uci_ptr ptr = {
			.package = "wireless", .section = s->e.name,
			.option = "disabled", .value = disabled ? "1" : "0",
		};

		if (!mode || strcmp(mode, "ap"))
			continue;

		uci_set(ctx, &ptr);
		changed = true;
	}

	if (changed) {
		uci_commit(ctx, &pkg, false);
		if (!ubus_lookup_id(ubus_ctx, "network", &id))
			ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
	}

	uci_free_context(ctx);
}

static void aps_ensure(bool up)
{
	if (aps_state_known && aps_down == !up)
		return;

	aps_set_disabled(!up);
	aps_down = !up;
	aps_state_known = true;

	roam_log(ROAM_L_INFO, "mesh: node access points %s", up ? "restored" : "shut down");
}

static bool cmd_line(const char *cmd, char *buf, size_t len)
{
	FILE *f = popen(cmd, "r");
	char line[128];
	bool got = false;

	buf[0] = '\0';
	if (!f)
		return false;

	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		strncpy(buf, line, len - 1);
		buf[len - 1] = '\0';
		got = buf[0] != '\0';
	}

	pclose(f);
	return got;
}

static const char *node_connection(void);
static const char *node_upstream_bssid(void);

static const char *bh_radio(struct uci_context *ctx, struct uci_package *pkg, int band)
{
	static const char *const want[] = { "2g", "5g" };
	struct uci_element *e;

	if (band < 0 || band > 1)
		return NULL;

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *b;

		if (strcmp(s->type, "wifi-device"))
			continue;

		b = uci_lookup_option_string(ctx, s, "band");
		if (b && !strcmp(b, want[band]))
			return s->e.name;
	}

	return NULL;
}

static bool bh_sta_apply(int band, bool enabled)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	const char *radio;
	uint32_t id;

	if (!mesh.backhaul_ssid[0] || !mesh.backhaul_key[0])
		return false;

	ctx = uci_alloc_context();
	if (!ctx)
		return false;

	if (uci_load(ctx, "wireless", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return false;
	}

	radio = bh_radio(ctx, pkg, band);
	if (!radio) {
		uci_free_context(ctx);
		return false;
	}

	if (!uci_lookup_section(ctx, pkg, MESH_BH_STA)) {
		struct uci_section *ns = NULL;

		uci_add_section(ctx, pkg, "wifi-iface", &ns);
		if (!ns) {
			uci_free_context(ctx);
			return false;
		}

		uci_rename(ctx, &(struct uci_ptr){
			.package = "wireless", .section = ns->e.name,
			.value = MESH_BH_STA });
	}

	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "device", radio);
	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "mode", "sta");
	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "network", "lan");
	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "encryption", "psk2");
	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "wds", "1");
	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "ssid", mesh.backhaul_ssid);
	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "key", mesh.backhaul_key);
	mesh_uci_set(ctx, "wireless", MESH_BH_STA, "disabled", enabled ? "0" : "1");

	uci_commit(ctx, &pkg, false);
	uci_free_context(ctx);

	if (!ubus_lookup_id(ubus_ctx, "network", &id))
		ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);

	roam_log(ROAM_L_INFO, "mesh: backhaul station %s on %s GHz",
		 enabled ? "up" : "down", band ? "5" : "2.4");

	return true;
}

static int bh_best_band(void)
{
	char cmd[768], out[32];
	int s24 = ROAMD_NO_SIGNAL, s5 = ROAMD_NO_SIGNAL;

	if (!mesh.backhaul_bssids[0])
		return -1;

	snprintf(cmd, sizeof(cmd),
		 "for d in $(iwinfo 2>/dev/null | sed -n 's/^\\([a-z0-9-]*\\) *ESSID.*/\\1/p'); do "
		 "ubus call iwinfo scan \"{\\\"device\\\":\\\"$d\\\"}\" 2>/dev/null; done | "
		 "awk -v want='%s' '"
		 "/\"bssid\":/ { gsub(/[\",]/, \"\"); b = toupper($2) } "
		 "/\"band\":/ { gsub(/[\",]/, \"\"); n = $2 } "
		 "/\"signal\":/ { gsub(/[\",]/, \"\"); "
		 "if (!index(toupper(want), b)) next; "
		 "if (n == 2 && (!h2 || $2 > b2)) { b2 = $2; h2 = 1 } "
		 "if (n == 5 && (!h5 || $2 > b5)) { b5 = $2; h5 = 1 } } "
		 "END { printf \"%%d %%d\", h2 ? b2 : 0, h5 ? b5 : 0 }'",
		 mesh.backhaul_bssids);

	if (!cmd_line(cmd, out, sizeof(out)))
		return -1;

	if (sscanf(out, "%d %d", &s24, &s5) != 2)
		return -1;

	if (s5 != ROAMD_NO_SIGNAL && (s24 == ROAMD_NO_SIGNAL ||
				      s5 >= s24 - config.cross_band_delta))
		return 1;

	return s24 != ROAMD_NO_SIGNAL ? 0 : -1;
}

static void bh_ensure(bool stale)
{
	int next;

	if (!stale) {
		if (!contact_seen)
			return;

		if (!bh_up)
			return;

		if (!mesh.backhaul_enabled) {
			bh_up = !bh_sta_apply(bh_band, false);
			return;
		}

		if (strcmp(node_connection(), "wifi"))
			bh_up = !bh_sta_apply(bh_band, false);
		return;
	}

	if (!mesh.backhaul_enabled)
		return;

	if (!bh_up) {
		int best = bh_best_band();
		int band;

		if (best >= 0 && bh_sta_apply(best, true)) {
			roam_log(ROAM_L_INFO, "mesh: backhaul band %s GHz chosen by signal",
				 best ? "5" : "2.4");
			bh_band = best;
			bh_up = true;
			bh_since = roam_now;
			return;
		}

		for (band = 0; band < 2; band++) {
			if (!bh_sta_apply(band, true))
				continue;

			bh_band = band;
			bh_up = true;
			bh_since = roam_now;
			break;
		}

		return;
	}

	if (roam_now - bh_since < MESH_BH_BAND_TIMEOUT)
		return;

	if (node_upstream_bssid()[0]) {
		bh_since = roam_now;
		return;
	}

	next = bh_band ? 0 : 1;
	if (bh_sta_apply(next, true))
		bh_band = next;

	bh_since = roam_now;
}

static char link_led[96];
static char link_led_max[16] = "255";
static bool link_led_known;
static int link_led_on = -1;

static void link_led_write(const char *node, const char *value)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", link_led, node);
	f = fopen(path, "w");
	if (!f)
		return;

	fputs(value, f);
	fclose(f);
}

static void link_led_read_max(void)
{
	char path[128], buf[16];
	FILE *f;

	snprintf(path, sizeof(path), "%s/max_brightness", link_led);
	f = fopen(path, "r");
	if (!f)
		return;

	if (fgets(buf, sizeof(buf), f)) {
		buf[strcspn(buf, "\r\n")] = '\0';
		if (buf[0])
			snprintf(link_led_max, sizeof(link_led_max), "%s", buf);
	}

	fclose(f);
}

static void link_led_find(void)
{
	char name[48];

	link_led_known = true;

	if (!cmd_line(
		"uci -q show system | sed -n \"s/.*\\.sysfs='\\(.*\\)'$/\\1/p\" | "
		"while read -r l; do case \"$l\" in *wan*|*internet*|*net*) "
		"[ -e \"/sys/class/leds/$l/brightness\" ] && { echo \"$l\"; break; };; esac; done",
		name, sizeof(name)) || !name[0])
		return;

	snprintf(link_led, sizeof(link_led), "/sys/class/leds/%s", name);

	link_led_read_max();
	link_led_write("trigger", "none");
	roam_log(ROAM_L_INFO, "mesh: link indicator %s", name);
}

static void link_led_set(bool on)
{
	if (!link_led_known)
		link_led_find();

	if (!link_led[0] || link_led_on == (int)on)
		return;

	link_led_write("brightness", on ? link_led_max : "0");
	link_led_on = on;
}

static void watch_cb(struct uloop_timeout *t)
{
	roam_time_update();

	if (mesh.role == MESH_NODE) {
		bool stale = last_contact &&
			(roam_now - last_contact) > MESH_CONTACT_TIMEOUT;

		link_led_set(contact_seen && !stale);
		bh_ensure(stale);

		if (mesh.wifi_shutdown)
			aps_ensure(!stale);
		else if (aps_state_known && aps_down)
			aps_ensure(true);
	} else if (aps_state_known && aps_down) {
		aps_ensure(true);
	}

	uloop_timeout_set(&watch_timer, MESH_WATCH_INTERVAL);
}

static void bh_state_load(void)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct uci_section *sta;
	const char *disabled, *device;

	ctx = uci_alloc_context();
	if (!ctx)
		return;

	if (uci_load(ctx, "wireless", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return;
	}

	sta = uci_lookup_section(ctx, pkg, MESH_BH_STA);
	disabled = sta ? uci_lookup_option_string(ctx, sta, "disabled") : NULL;

	if (sta && !roam_uci_bool(disabled)) {
		bh_up = true;
		bh_since = roam_now;

		device = uci_lookup_option_string(ctx, sta, "device");
		if (device && bh_radio(ctx, pkg, 1) &&
		    !strcmp(device, bh_radio(ctx, pkg, 1)))
			bh_band = 1;
	}

	uci_free_context(ctx);
}

uint32_t mesh_node_contact_age(void)
{
	if (!last_contact || roam_now < last_contact)
		return 0;

	return (uint32_t)((roam_now - last_contact) / 1000);
}

void mesh_node_watch_start(void)
{
	uint64_t grace = MESH_CONTACT_TIMEOUT / 2;

	last_contact = roam_now > grace ? roam_now - grace : 1;
	bh_state_load();

	if (watch_timer.cb)
		return;

	watch_timer.cb = watch_cb;
	uloop_timeout_set(&watch_timer, MESH_WATCH_INTERVAL);
}

static void uci_set_opt(struct uci_context *ctx, const char *section,
			const char *option, const char *value)
{
	mesh_uci_set(ctx, "roamd", section, option, value);
}

static void apply_table(struct uci_context *ctx, const char *section,
			struct blob_attr *table)
{
	struct blob_attr *cur;
	int rem;

	if (!table)
		return;

	blobmsg_for_each_attr(cur, table, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			continue;
		uci_set_opt(ctx, section, blobmsg_name(cur), blobmsg_get_string(cur));
	}
}

enum {
	APPLY_CONTROLLER_ID,
	APPLY_GLOBAL,
	APPLY_POLICY,
	APPLY_BACKHAUL,
	APPLY_DEVICES,
	APPLY_WIFI,
	APPLY_CREDS,
	__APPLY_MAX
};

static const struct blobmsg_policy apply_policy[__APPLY_MAX] = {
	[APPLY_CONTROLLER_ID] = { .name = "controller_id", .type = BLOBMSG_TYPE_STRING },
	[APPLY_GLOBAL] = { .name = "global", .type = BLOBMSG_TYPE_TABLE },
	[APPLY_POLICY] = { .name = "policy", .type = BLOBMSG_TYPE_TABLE },
	[APPLY_BACKHAUL] = { .name = "backhaul", .type = BLOBMSG_TYPE_TABLE },
	[APPLY_DEVICES] = { .name = "devices", .type = BLOBMSG_TYPE_ARRAY },
	[APPLY_WIFI] = { .name = "wifi", .type = BLOBMSG_TYPE_TABLE },
	[APPLY_CREDS] = { .name = "credentials", .type = BLOBMSG_TYPE_TABLE },
};

static bool apply_credentials(struct blob_attr *cred)
{
	static const struct blobmsg_policy cp = { .name = "root_hash", .type = BLOBMSG_TYPE_STRING };
	struct blob_attr *rh = NULL;
	const char *hash;
	FILE *in, *out;
	char line[512];
	bool changed = false, wrote = false;

	if (!cred)
		return false;

	blobmsg_parse(&cp, 1, &rh, blobmsg_data(cred), blobmsg_data_len(cred));
	if (!rh)
		return false;
	hash = blobmsg_get_string(rh);

	in = fopen("/etc/shadow", "r");
	if (!in)
		return false;
	out = fopen("/etc/shadow.roamd", "w");
	if (!out) {
		fclose(in);
		return false;
	}

	while (fgets(line, sizeof(line), in)) {
		if (!strncmp(line, "root:", 5)) {
			char *p1 = line + 5;
			char *p2 = strchr(p1, ':');

			if (p2) {
				size_t len = p2 - p1;

				if (strlen(hash) != len || strncmp(p1, hash, len))
					changed = true;
				fprintf(out, "root:%s%s", hash, p2);
				wrote = true;
				continue;
			}
		}
		fputs(line, out);
	}

	fclose(in);
	fclose(out);

	if (wrote && changed) {
		chmod("/etc/shadow.roamd", 0600);
		rename("/etc/shadow.roamd", "/etc/shadow");
		return true;
	}

	unlink("/etc/shadow.roamd");
	return false;
}

enum { WIFI_SSID, WIFI_ENC, WIFI_KEY, __WIFI_MAX };

static const struct blobmsg_policy wifi_policy[__WIFI_MAX] = {
	[WIFI_SSID] = { .name = "ssid", .type = BLOBMSG_TYPE_STRING },
	[WIFI_ENC] = { .name = "encryption", .type = BLOBMSG_TYPE_STRING },
	[WIFI_KEY] = { .name = "key", .type = BLOBMSG_TYPE_STRING },
};

static bool apply_wifi(struct blob_attr *wifi)
{
	static const char *const names[] = { "ssid", "encryption", "key" };
	struct blob_attr *tb[__WIFI_MAX];
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	bool changed = false;

	if (!wifi)
		return false;

	blobmsg_parse(wifi_policy, __WIFI_MAX, tb, blobmsg_data(wifi), blobmsg_data_len(wifi));
	if (!tb[WIFI_SSID])
		return false;

	ctx = uci_alloc_context();
	if (!ctx)
		return false;

	if (uci_load(ctx, "wireless", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return false;
	}

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *mode = uci_lookup_option_string(ctx, s, "mode");
		size_t i;

		if (!strcmp(s->type, "wifi-device")) {
			if (roam_uci_bool(uci_lookup_option_string(ctx, s, "disabled"))) {
				mesh_uci_set(ctx, "wireless", s->e.name, "disabled", "0");
				changed = true;
			}
			continue;
		}

		if (!mode || strcmp(mode, "ap"))
			continue;

		if (!strncmp(s->e.name, MESH_BH_PREFIX, strlen(MESH_BH_PREFIX)))
			continue;

		if (roam_uci_bool(uci_lookup_option_string(ctx, s, "disabled"))) {
			mesh_uci_set(ctx, "wireless", s->e.name, "disabled", "0");
			changed = true;
		}

		for (i = 0; i < __WIFI_MAX; i++) {
			const char *cur = uci_lookup_option_string(ctx, s, names[i]);
			struct uci_ptr ptr = {
				.package = "wireless", .section = s->e.name,
				.option = names[i],
			};

			if (!tb[i])
				continue;
			ptr.value = blobmsg_get_string(tb[i]);
			if (cur && !strcmp(cur, ptr.value))
				continue;
			uci_set(ctx, &ptr);
			changed = true;
		}
	}

	if (changed)
		uci_commit(ctx, &pkg, false);

	uci_free_context(ctx);
	return changed;
}

enum { DEV_MAC, DEV_BAND, DEV_NODES, __DEV_MAX };

static const struct blobmsg_policy dev_policy[__DEV_MAX] = {
	[DEV_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[DEV_BAND] = { .name = "band", .type = BLOBMSG_TYPE_STRING },
	[DEV_NODES] = { .name = "nodes", .type = BLOBMSG_TYPE_ARRAY },
};

static void apply_devices(struct uci_context *ctx, struct uci_package *pkg,
			  struct blob_attr *devices)
{
	struct uci_element *e, *tmp;
	struct blob_attr *cur;
	int rem;

	uci_foreach_element_safe(&pkg->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);
		struct uci_ptr ptr = { .package = "roamd", .section = s->e.name };

		if (!strcmp(s->type, "device") &&
		    uci_lookup_ptr(ctx, &ptr, NULL, false) == UCI_OK)
			uci_delete(ctx, &ptr);
	}

	if (!devices)
		return;

	blobmsg_for_each_attr(cur, devices, rem) {
		struct blob_attr *tb[__DEV_MAX];
		struct uci_section *ns = NULL;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE)
			continue;

		blobmsg_parse(dev_policy, __DEV_MAX, tb,
			      blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[DEV_MAC])
			continue;

		uci_add_section(ctx, pkg, "device", &ns);
		if (!ns)
			continue;

		uci_set(ctx, &(struct uci_ptr){
			.package = "roamd", .section = ns->e.name,
			.option = "mac", .value = blobmsg_get_string(tb[DEV_MAC]) });

		if (tb[DEV_BAND])
			uci_set(ctx, &(struct uci_ptr){
				.package = "roamd", .section = ns->e.name,
				.option = "band", .value = blobmsg_get_string(tb[DEV_BAND]) });

		if (tb[DEV_NODES]) {
			struct blob_attr *n;
			int nrem;

			blobmsg_for_each_attr(n, tb[DEV_NODES], nrem)
				if (blobmsg_type(n) == BLOBMSG_TYPE_STRING)
					uci_add_list(ctx, &(struct uci_ptr){
						.package = "roamd", .section = ns->e.name,
						.option = "node", .value = blobmsg_get_string(n) });
		}
	}
}

bool mesh_node_apply(struct blob_attr *msg)
{
	struct blob_attr *tb[__APPLY_MAX];
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	const char *from;

	blobmsg_parse(apply_policy, __APPLY_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[APPLY_CONTROLLER_ID])
		return false;

	from = blobmsg_get_string(tb[APPLY_CONTROLLER_ID]);
	if (mesh.controller_id[0] && strcmp(from, mesh.controller_id))
		return false;

	ctx = uci_alloc_context();
	if (!ctx)
		return false;

	if (uci_load(ctx, "roamd", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return false;
	}

	apply_table(ctx, "global", tb[APPLY_GLOBAL]);
	apply_table(ctx, "policy", tb[APPLY_POLICY]);
	apply_table(ctx, "mesh", tb[APPLY_BACKHAUL]);
	apply_devices(ctx, pkg, tb[APPLY_DEVICES]);

	uci_commit(ctx, &pkg, false);
	uci_free_context(ctx);

	if (apply_wifi(tb[APPLY_WIFI])) {
		uint32_t id;

		if (!ubus_lookup_id(ubus_ctx, "network", &id))
			ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
	}

	apply_credentials(tb[APPLY_CREDS]);

	mesh_node_touch();
	roam_config_load();
	roam_wireless_apply();
	roam_bss_recheck();

	return true;
}

static void node_profile(struct blob_buf *b)
{
	static const char *const fields[] = {
		"ssid", "encryption", "mobility_domain",
		"ieee80211r", "bss_transition", "ieee80211k"
	};
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	void *t;

	if (!ctx)
		return;

	if (uci_load(ctx, "wireless", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return;
	}

	t = blobmsg_open_table(b, "profile");

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *mode = uci_lookup_option_string(ctx, s, "mode");
		size_t i;

		if (!mode || strcmp(mode, "ap"))
			continue;

		for (i = 0; i < ARRAY_SIZE(fields); i++) {
			const char *v = uci_lookup_option_string(ctx, s, fields[i]);

			if (v)
				blobmsg_add_string(b, fields[i], v);
		}
		break;
	}

	if (mesh.backhaul_ssid[0])
		blobmsg_add_string(b, "backhaul_ssid", mesh.backhaul_ssid);

	blobmsg_close_table(b, t);
	uci_free_context(ctx);
}

#define STA_IFACES \
	"for d in /sys/class/net/*/brport; do i=${d%/brport}; i=${i##*/}; " \
	"[ -d \"/sys/class/net/$i/phy80211\" ] || continue; "


static const char *node_upstream_bssid(void)
{
	static char bssid[18];

	cmd_line(STA_IFACES
		 "iw dev \"$i\" link 2>/dev/null | "
		 "sed -n 's/^Connected to \\([0-9a-f:]*\\).*/\\1/p'; done | head -1",
		 bssid, sizeof(bssid));

	return bssid;
}

static const char *node_via(void)
{
	if (strcmp(node_connection(), "wifi"))
		return "controller";

	return node_upstream_bssid();
}

static bool node_uplink(struct mesh_uplink *up)
{
	uint8_t mac[6];

	return mesh_parent_mac(mac) && mesh_bridge_port_of(mac, up);
}

static const char *node_uplink_band(void)
{
	struct mesh_uplink up;
	char cmd[160], freq[16];

	if (!node_uplink(&up) || !up.wireless)
		return "";

	snprintf(cmd, sizeof(cmd),
		 "iw dev %s link 2>/dev/null | sed -n 's/^\tfreq: \\([0-9]*\\).*/\\1/p'",
		 up.ifname);

	if (!cmd_line(cmd, freq, sizeof(freq)) || !freq[0])
		return "";

	return atoi(freq) < 3000 ? "2.4" : "5";
}

static const char *node_connection(void)
{
	struct mesh_uplink up;
	char state[8];

	if (node_uplink(&up))
		return up.wireless ? "wifi" : "wired";

	cmd_line(STA_IFACES
		 "iw dev \"$i\" link 2>/dev/null | grep -q '^Connected to' || continue; "
		 "[ \"$(cat \"$d/state\")\" = 3 ] && { echo wifi; exit; }; done",
		 state, sizeof(state));

	return state[0] ? "wifi" : "wired";
}

static void release_field(const char *key, char *dst, size_t size)
{
	FILE *f = fopen("/etc/openwrt_release", "r");
	char line[128];

	dst[0] = '\0';
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *v = strstr(line, key);

		if (v) {
			v += strlen(key);
			while (*v == '\'' || *v == '"')
				v++;
			strncpy(dst, v, size - 1);
			dst[size - 1] = '\0';
			dst[strcspn(dst, "'\"\r\n")] = '\0';
			break;
		}
	}

	fclose(f);
}

static bool version_from_db(const char *path, const char *pkg_key, const char *ver_key,
			    const char *name, char *dst, size_t size)
{
	size_t pkg_len = strlen(pkg_key), ver_len = strlen(ver_key), name_len = strlen(name);
	FILE *f = fopen(path, "r");
	char line[256];
	bool in_pkg = false;

	if (!f)
		return false;

	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, pkg_key, pkg_len)) {
			in_pkg = !strncmp(line + pkg_len, name, name_len) &&
				 line[pkg_len + name_len] == '\n';
			continue;
		}

		if (in_pkg && !strncmp(line, ver_key, ver_len)) {
			strncpy(dst, line + ver_len, size - 1);
			dst[size - 1] = '\0';
			dst[strcspn(dst, "\r\n")] = '\0';
			break;
		}
	}

	fclose(f);
	return dst[0] != '\0';
}

static void package_version(const char *name, char *dst, size_t size)
{
	dst[0] = '\0';

	if (version_from_db("/usr/lib/opkg/status", "Package: ", "Version: ", name, dst, size))
		return;

	version_from_db("/lib/apk/db/installed", "P:", "V:", name, dst, size);
}

static void diag_cmd(struct blob_buf *b, const char *name, const char *cmd)
{
	char line[512];
	FILE *f;
	void *arr;

	f = popen(cmd, "r");
	if (!f)
		return;

	arr = blobmsg_open_array(b, name);
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0])
			blobmsg_add_string(b, NULL, line);
	}
	blobmsg_close_array(b, arr);

	pclose(f);
}

void mesh_node_diag(struct blob_buf *b)
{
	char value[MESH_NAME_MAX];

	release_field("DISTRIB_RELEASE=", value, sizeof(value));
	blobmsg_add_string(b, "os_version", value);
	release_field("DISTRIB_ARCH=", value, sizeof(value));
	blobmsg_add_string(b, "arch", value);
	package_version("roamd", value, sizeof(value));
	blobmsg_add_string(b, "pkg_version", value);

	package_version("luci-app-roamd", value, sizeof(value));
	blobmsg_add_string(b, "ui_version", value);

	diag_cmd(b, "hostapd_pkg",
		 "(apk info 2>/dev/null || opkg list-installed 2>/dev/null | awk '{print $1}') | "
		 "grep -xE '(wpad|hostapd)(-(mini|basic|mesh))?(-(openssl|mbedtls|wolfssl))?'");
	diag_cmd(b, "wireless", "uci show wireless 2>/dev/null | grep -v '\\.key='");
	diag_cmd(b, "network", "uci show network 2>/dev/null | grep -vE 'private_key|psk'");
	diag_cmd(b, "dhcp_server", "uci show dhcp 2>/dev/null | grep -E 'ignore|dhcpv4|dhcpv6|\\.ra='");
	diag_cmd(b, "services", "pgrep -l 'dnsmasq|odhcpd|hostapd|wpa_supplicant|dropbear' 2>/dev/null");
	diag_cmd(b, "hostapd_objects", "ubus list 2>/dev/null | grep '^hostapd'");
	diag_cmd(b, "addresses", "ip -o -4 addr show 2>/dev/null | awk '{print $2, $4}'");
	diag_cmd(b, "bridge", "brctl show 2>/dev/null");
	diag_cmd(b, "nat_rules", "nft list ruleset 2>/dev/null | grep -c masquerade");
	diag_cmd(b, "dropbear_conf", "uci show dropbear 2>/dev/null");
	diag_cmd(b, "authorized_keys", "cut -c1-45 /etc/dropbear/authorized_keys 2>/dev/null");
	diag_cmd(b, "roamd_log", "tail -n 60 /tmp/roamd-diag.log 2>/dev/null");
	diag_cmd(b, "install_log", "tail -n 20 /tmp/roamd-install.log 2>/dev/null");
	diag_cmd(b, "syslog",
		 "logread 2>/dev/null | grep -iE 'hostapd|dropbear|roamd|netifd|wpad|udhcpc' | tail -n 90");
}

bool mesh_node_steer(const char *macstr, struct blob_attr *neighbors)
{
	static struct blob_buf sb;
	struct ether_addr *ea = ether_aton(macstr);
	struct roam_sta *sta;

	if (!ea)
		return false;

	sta = roam_sta_get(ea->ether_addr_octet, false);
	if (!sta || !sta->bss)
		return false;

	if (!sta->btm) {
		if (!config.allow_kick)
			return false;

		roam_log(ROAM_L_INFO, "mesh: %s cannot be asked to move, disconnecting", sta->mac);
		roam_policy_kick(sta, sta->bss);

		return true;
	}

	blob_buf_init(&sb, 0);
	blobmsg_add_string(&sb, "addr", sta->mac);
	blobmsg_add_u8(&sb, "disassociation_imminent", config.allow_kick);
	blobmsg_add_u32(&sb, "disassociation_timer", 0);
	blobmsg_add_u32(&sb, "reassoc_delay", 0);
	blobmsg_add_u32(&sb, "mbo_reason", 5);
	blobmsg_add_u32(&sb, "cell_pref", 0);
	blobmsg_add_u8(&sb, "abridged", 1);

	if (neighbors) {
		struct blob_attr *cur;
		void *arr;
		int rem;

		arr = blobmsg_open_array(&sb, "neighbors");
		blobmsg_for_each_attr(cur, neighbors, rem)
			if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
				blobmsg_add_string(&sb, NULL, blobmsg_get_string(cur));
		blobmsg_close_array(&sb, arr);
	}

	roam_bss_invoke(sta->bss, "bss_transition_request", &sb);

	sta->last_steer = roam_now;
	sta->steer_count++;

	return true;
}

void mesh_self_info(struct blob_buf *b)
{
	struct sysinfo si;
	char value[32];

	release_field("DISTRIB_RELEASE=", value, sizeof(value));
	blobmsg_add_string(b, "os_version", value);
	release_field("DISTRIB_ARCH=", value, sizeof(value));
	blobmsg_add_string(b, "arch", value);

	package_version("roamd", value, sizeof(value));
	blobmsg_add_string(b, "pkg_version", value);
	package_version("luci-app-roamd", value, sizeof(value));
	blobmsg_add_string(b, "ui_version", value);

	if (!sysinfo(&si))
		blobmsg_add_u32(b, "uptime", si.uptime);

	if (cmd_line("ip -4 -o addr show scope global 2>/dev/null | "
		     "awk '$2 !~ /:/ { split($4, a, \"/\"); print a[1]; exit }'",
		     value, sizeof(value)))
		blobmsg_add_string(b, "addr", value);

	if (cmd_line("cat /sys/class/net/br-lan/address 2>/dev/null", value, sizeof(value)))
		blobmsg_add_string(b, "mac", value);
}

void mesh_node_report(struct blob_buf *b)
{
	struct sysinfo si;
	struct roam_sta *sta;
	struct mesh_assoc_idx assoc;
	uint8_t pmac[6];
	char version[32];
	void *clients;
	unsigned int count = 0;

	mesh_node_touch();
	mesh_assoc_collect(&assoc);
	if (mesh_parent_mac(pmac))
		mesh_wired_collect(&assoc, pmac);

	blobmsg_add_string(b, "role", mesh_role_name(mesh.role));
	blobmsg_add_string(b, "member_id", mesh.member_id);
	blobmsg_add_string(b, "controller_id", mesh.controller_id);

	release_field("DISTRIB_RELEASE=", version, sizeof(version));
	blobmsg_add_string(b, "os_version", version);
	release_field("DISTRIB_ARCH=", version, sizeof(version));
	blobmsg_add_string(b, "arch", version);
	blobmsg_add_string(b, "roamd_version", ROAMD_VERSION);
	package_version("roamd", version, sizeof(version));
	blobmsg_add_string(b, "pkg_version", version);
	package_version("luci-app-roamd", version, sizeof(version));
	blobmsg_add_string(b, "ui_version", version);

	if (!sysinfo(&si))
		blobmsg_add_u32(b, "uptime", si.uptime);

	clients = blobmsg_open_array(b, "clients");
	for (count = 0; count < assoc.n; count++) {
		const struct mesh_assoc *a = &assoc.e[count];
		void *e = blobmsg_open_table(b, NULL);
		char mac[18];

		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
			 a->addr[0], a->addr[1], a->addr[2],
			 a->addr[3], a->addr[4], a->addr[5]);

		sta = roam_sta_get(a->addr, false);

		blobmsg_add_string(b, "mac", mac);
		if (!a->wired) {
			blobmsg_add_string(b, "band", roam_band_name(a->band));
			if (sta && sta->bss)
				blobmsg_add_u32(b, "signal", sta->band[sta->bss->band].signal);
		}
		mesh_assoc_blob(b, a);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, clients);

	blobmsg_add_u32(b, "client_count", count);

	clients = blobmsg_open_array(b, "heard");
	avl_for_each_element(&roam_sta_tree, sta, avl) {
		enum roam_band band;
		int best = ROAMD_NO_SIGNAL;
		void *e;

		if (sta->bss)
			continue;

		for (band = 0; band < BAND_MAX; band++) {
			const struct roam_sta_band *info = &sta->band[band];

			if (!info->present || info->signal == ROAMD_NO_SIGNAL)
				continue;
			if (roam_now - info->seen > config.age_time)
				continue;
			if (best == ROAMD_NO_SIGNAL || info->signal > best)
				best = info->signal;
		}

		if (best == ROAMD_NO_SIGNAL)
			continue;

		e = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "mac", sta->mac);
		blobmsg_add_u32(b, "signal", best);
		blobmsg_close_table(b, e);
	}
	blobmsg_close_array(b, clients);
	blobmsg_add_string(b, "connection", node_connection());
	blobmsg_add_string(b, "uplink_band", node_uplink_band());
	blobmsg_add_string(b, "via", node_via());

	node_profile(b);
	mesh_aps_dump(b, "aps");
	mesh_log_dump(b, 100);
}
