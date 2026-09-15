#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/ether.h>

#include <libubox/uloop.h>

#include "roamd.h"
#include "mesh.h"

#define MESH_WATCH_INTERVAL	5000
#define MESH_CONTACT_MISS	15000
#define MESH_LINK_GRACE_AIR	20000
#define MESH_LINK_GRACE_CABLE	10000
#define MESH_UPSTREAM_PATIENCE	120000
#define MESH_PARENT_PROBATION	60000
#define MESH_PARENT_PENALTY	600000
#define MESH_CABLE_HOLD		30000
#define MESH_CABLE_PROOF	120000
#define MESH_CABLE_PENALTY	600000
#define MESH_WEAK_HOLD		600000
#define MESH_RESCAN_GAP		600000
#define MESH_LINK_SAMPLE	60000
#define MESH_BH_SCAN_RETRY	30000
#define MESH_SIGNAL_STRONG	-65
#define MESH_PROBE_ID		0x524d
#define MESH_BRIDGE		"br-lan"
#define MESH_CONTROLLER_OWNER	"controller"
#define MESH_BH_STA		"mesh_bh_sta"
#define MESH_BH_SCAN		"/usr/libexec/roamd/mesh-bhscan.sh"
#define MESH_BH_SCAN_OUT	"/tmp/roamd/bh_scan"
#define MESH_BH_WANT_LEN	(MESH_BH_PARENT_MAX * MESH_MAC_MAX + 1)
#define MESH_DROP_REASON	4
#define MESH_DUMBAP		"/usr/libexec/roamd/mesh-dumbap.sh"

#define STA_IFACES \
	"for d in /sys/class/net/*/brport; do i=${d%/brport}; i=${i##*/}; " \
	"[ -d \"/sys/class/net/$i/phy80211\" ] || continue; "

enum bh_class {
	BH_WEAK,
	BH_USABLE,
	BH_STRONG
};

struct bh_parent {
	char bssid[MESH_MAC_MAX];
	const char *chain;
	int depth;
	int band;
	int signal;
	unsigned int samples;
};

struct bh_penalty {
	char owner[MESH_ID_MAX];
	uint64_t until;
};

static void bh_scan_done(struct mesh_task *task, int ret);

static struct uloop_timeout watch_timer;
static struct mesh_task dumbap_job;
static struct mesh_task bh_scan_job = { .done = bh_scan_done };
static uint64_t last_contact;
static uint64_t link_down_since;
static uint64_t bh_since;
static uint64_t bh_scan_at;
static uint64_t weak_since;
static uint64_t link_sample_at;
static uint64_t rescan_at;
static uint64_t cable_since;
static uint64_t cable_dropped_at;
static uint64_t cable_penalty_until;
static bool bh_scan_empty;
static bool bh_scan_rescan;
static bool bh_want_search;
static bool bh_weak_attach;
static bool aps_down;
static bool aps_state_known;
static bool bh_up;
static int bh_band;
static int bh_bss_on = -1;
static char bh_parent[MESH_MAC_MAX];
static char bh_parent_owner[MESH_ID_MAX];
static char uplink_port[sizeof(((struct mesh_uplink *)0)->ifname)];
static char bh_parents_seen[MESH_BH_PARENTS_LEN];
static char bh_parents_buf[MESH_BH_PARENTS_LEN];
static struct bh_parent bh_parents[MESH_BH_PARENT_MAX];
static struct bh_penalty bh_penalties[MESH_BH_PARENT_MAX];
static unsigned int bh_parents_n;

void mesh_node_dumbap(void)
{
	mesh_task_start(&dumbap_job, MESH_DUMBAP, NULL, NULL);
}

void mesh_node_touch(void)
{
	last_contact = roam_now;
}

static bool contact_lost(void)
{
	return roam_now - last_contact > MESH_CONTACT_MISS;
}

static bool client_ap(struct uci_context *ctx, struct uci_section *s)
{
	const char *mode = uci_lookup_option_string(ctx, s, "mode");

	return mode && !strcmp(mode, "ap") &&
	       strncmp(s->e.name, MESH_BH_PREFIX, strlen(MESH_BH_PREFIX));
}

static void aps_set_disabled(bool disabled)
{
	struct uci_session u;
	struct uci_element *e;
	uint32_t id;

	if (!uci_session_open(&u, "wireless"))
		return;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (!client_ap(u.ctx, s))
			continue;

		uci_session_set(&u, s->e.name, "disabled", disabled ? "1" : "0");
	}

	if (u.dirty && !ubus_lookup_id(ubus_ctx, "network", &id)) {
		uci_session_close(&u);
		ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
		return;
	}

	uci_session_close(&u);
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
static bool node_uplink(struct mesh_uplink *up);
static bool node_wired_uplink(void);

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

static bool bh_sta_apply(int band, const char *bssid, bool enabled)
{
	struct uci_session u;
	const char *radio;
	bool changed;
	uint32_t id;

	if (!mesh.backhaul_ssid[0] || !mesh.backhaul_key[0])
		return false;

	if (!uci_session_open(&u, "wireless"))
		return false;

	radio = bh_radio(u.ctx, u.pkg, band);
	if (!radio || !uci_session_add(&u, "wifi-iface", MESH_BH_STA)) {
		uci_session_close(&u);
		return false;
	}

	uci_session_set(&u, MESH_BH_STA, "device", radio);
	uci_session_set(&u, MESH_BH_STA, "mode", "sta");
	uci_session_set(&u, MESH_BH_STA, "network", "lan");
	uci_session_set(&u, MESH_BH_STA, "encryption", "psk2");
	uci_session_set(&u, MESH_BH_STA, "wds", "1");
	uci_session_set(&u, MESH_BH_STA, "ssid", mesh.backhaul_ssid);
	uci_session_set(&u, MESH_BH_STA, "key", mesh.backhaul_key);
	if (bssid[0])
		uci_session_set(&u, MESH_BH_STA, "bssid", bssid);
	uci_session_set(&u, MESH_BH_STA, "disabled", enabled ? "0" : "1");

	changed = u.dirty;
	uci_session_close(&u);

	if (!changed)
		return true;

	if (!ubus_lookup_id(ubus_ctx, "network", &id))
		ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);

	roam_log(ROAM_L_INFO, "mesh: backhaul station %s on %s GHz",
		 enabled ? "up" : "down", band ? "5" : "2.4");

	return true;
}

static bool bh_chain_has(const char *chain, const char *id)
{
	size_t len = strlen(id);
	const char *p = chain;

	while (len && (p = strstr(p, id))) {
		if ((p == chain || p[-1] == '-') && (p[len] == '\0' || p[len] == '-'))
			return true;
		p += len;
	}

	return false;
}

static void bh_parents_load(void)
{
	struct uci_session u;
	bool radio[2] = { false, false };
	char *save, *tok;

	if (uci_session_open(&u, "wireless")) {
		radio[0] = bh_radio(u.ctx, u.pkg, 0) != NULL;
		radio[1] = bh_radio(u.ctx, u.pkg, 1) != NULL;
		uci_session_close(&u);
	}

	bh_parents_n = 0;
	snprintf(bh_parents_buf, sizeof(bh_parents_buf), "%s", mesh.backhaul_parents);

	for (tok = strtok_r(bh_parents_buf, " ", &save);
	     tok && bh_parents_n < MESH_BH_PARENT_MAX;
	     tok = strtok_r(NULL, " ", &save)) {
		struct bh_parent *p = &bh_parents[bh_parents_n];
		char *depth = strchr(tok, '/');
		char *chain = depth ? strchr(depth + 1, '/') : NULL;
		char *band = chain ? strchr(chain + 1, '/') : NULL;

		if (!band)
			continue;

		*depth++ = '\0';
		*chain++ = '\0';
		*band++ = '\0';

		p->band = !strcmp(band, "5");
		if (!radio[p->band] || bh_chain_has(chain, mesh.member_id))
			continue;

		snprintf(p->bssid, sizeof(p->bssid), "%s", tok);
		p->chain = chain;
		p->depth = atoi(depth);
		p->signal = ROAMD_NO_SIGNAL;
		p->samples = 0;
		bh_parents_n++;
	}
}

static const char *bh_owner(const struct bh_parent *p)
{
	const char *dash = strrchr(p->chain, '-');

	if (dash)
		return dash + 1;

	return p->chain[0] ? p->chain : MESH_CONTROLLER_OWNER;
}

static void bh_owner_restore(void)
{
	unsigned int i;

	bh_parent_owner[0] = '\0';
	bh_parents_load();

	for (i = 0; i < bh_parents_n; i++) {
		if (strcasecmp(bh_parents[i].bssid, bh_parent))
			continue;

		snprintf(bh_parent_owner, sizeof(bh_parent_owner), "%s", bh_owner(&bh_parents[i]));
		return;
	}
}

static bool bh_penalized(const char *owner)
{
	unsigned int i;

	for (i = 0; i < MESH_BH_PARENT_MAX; i++)
		if (bh_penalties[i].until > roam_now && !strcmp(bh_penalties[i].owner, owner))
			return true;

	return false;
}

static void bh_penalize(const char *why)
{
	struct bh_penalty *slot = &bh_penalties[0];
	unsigned int i;

	if (!bh_parent_owner[0])
		return;

	for (i = 0; i < MESH_BH_PARENT_MAX; i++) {
		if (!strcmp(bh_penalties[i].owner, bh_parent_owner)) {
			slot = &bh_penalties[i];
			break;
		}

		if (bh_penalties[i].until < slot->until)
			slot = &bh_penalties[i];
	}

	snprintf(slot->owner, sizeof(slot->owner), "%s", bh_parent_owner);
	slot->until = roam_now + MESH_PARENT_PENALTY;

	roam_log(ROAM_L_INFO, "mesh: backhaul parent %s %s, avoided for 10 minutes", bh_parent, why);
}

static bool bh_scan_start(bool rescan)
{
	char want[MESH_BH_WANT_LEN];
	size_t off = 0;
	unsigned int i;

	for (i = 0; i < bh_parents_n; i++)
		off += snprintf(want + off, sizeof(want) - off, "%s ", bh_parents[i].bssid);

	if (!off)
		return false;

	unlink(MESH_BH_SCAN_OUT);
	if (!mesh_task_start(&bh_scan_job, MESH_BH_SCAN, MESH_BH_SCAN_OUT, want))
		return false;

	bh_scan_rescan = rescan;

	return true;
}

static bool bh_scan_read(void)
{
	FILE *f = fopen(MESH_BH_SCAN_OUT, "r");
	char line[64];
	bool heard = false;

	if (!f)
		return false;

	while (fgets(line, sizeof(line), f)) {
		char bssid[MESH_MAC_MAX];
		unsigned int i;
		int signal;

		if (sscanf(line, "%17s %d", bssid, &signal) != 2)
			continue;

		for (i = 0; i < bh_parents_n; i++) {
			struct bh_parent *p = &bh_parents[i];

			if (strcasecmp(p->bssid, bssid))
				continue;

			if (!p->samples || signal < p->signal)
				p->signal = signal;
			p->samples++;
			heard = true;
		}
	}

	fclose(f);

	return heard;
}

static enum bh_class bh_class_of(int signal, bool stable)
{
	if (!stable || signal < mesh.backhaul_min_signal)
		return BH_WEAK;

	return signal >= MESH_SIGNAL_STRONG ? BH_STRONG : BH_USABLE;
}

static enum bh_class bh_class(const struct bh_parent *p)
{
	return bh_class_of(p->signal, p->samples > 1);
}

static int bh_band_pick(int i24, int i5)
{
	if (i5 < 0)
		return i24;

	if (i24 < 0 || (bh_parents[i5].signal >= mesh.backhaul_min_signal &&
			bh_parents[i5].signal >= bh_parents[i24].signal - mesh.backhaul_delta))
		return i5;

	return i24;
}

static bool bh_better(int a, int b)
{
	const struct bh_parent *pa = &bh_parents[a], *pb;
	enum bh_class ca, cb;

	if (b < 0)
		return true;

	pb = &bh_parents[b];
	ca = bh_class(pa);
	cb = bh_class(pb);

	if (ca != cb)
		return ca > cb;

	if (pa->depth != pb->depth)
		return pa->depth < pb->depth;

	return pa->signal > pb->signal;
}

static int bh_choose(bool allow_penalized)
{
	bool done[MESH_BH_PARENT_MAX] = { false };
	unsigned int i, j;
	int best = -1;

	for (i = 0; i < bh_parents_n; i++) {
		bool skip = !allow_penalized && bh_penalized(bh_owner(&bh_parents[i]));
		int i24 = -1, i5 = -1, pick;

		if (done[i])
			continue;

		for (j = i; j < bh_parents_n; j++) {
			const struct bh_parent *p = &bh_parents[j];
			int *slot;

			if (strcmp(p->chain, bh_parents[i].chain))
				continue;

			done[j] = true;
			if (skip || !p->samples)
				continue;

			slot = p->band ? &i5 : &i24;
			if (*slot < 0 || p->signal > bh_parents[*slot].signal)
				*slot = j;
		}

		pick = bh_band_pick(i24, i5);
		if (pick >= 0 && bh_better(pick, best))
			best = pick;
	}

	return best;
}

static int bh_select(void)
{
	int best = bh_choose(false);

	return best >= 0 ? best : bh_choose(true);
}

static void bh_attach(int idx, const char *why)
{
	const struct bh_parent *p = &bh_parents[idx];

	if (!bh_sta_apply(p->band, p->bssid, true))
		return;

	roam_log(ROAM_L_INFO, "mesh: backhaul parent %s %s: %s GHz, %d dBm, %d hop(s) from the controller",
		 p->bssid, why, p->band ? "5" : "2.4", p->signal, p->depth + 1);

	snprintf(bh_parent, sizeof(bh_parent), "%s", p->bssid);
	snprintf(bh_parent_owner, sizeof(bh_parent_owner), "%s", bh_owner(p));
	bh_band = p->band;
	bh_up = true;
	bh_since = roam_now;
	link_down_since = roam_now;
	link_sample_at = roam_now;
	bh_want_search = false;
	bh_scan_empty = false;
	cable_since = 0;
	weak_since = 0;
	bh_weak_attach = bh_class(p) == BH_WEAK;
}

static void bh_detach(bool search)
{
	bh_up = !bh_sta_apply(bh_band, bh_parent, false);
	bh_want_search = search;
	cable_since = 0;
}

static void bh_no_parent(void)
{
	if (!bh_scan_empty)
		roam_log(ROAM_L_INFO, "mesh: no backhaul parent in range");

	bh_scan_empty = true;
}

static bool bh_no_path(void)
{
	return contact_lost() &&
	       (bh_scan_empty || roam_now - last_contact >= MESH_UPSTREAM_PATIENCE);
}

static void bh_bss_ensure(bool relay)
{
	bool up = relay && mesh.backhaul_enabled &&
		  mesh.backhaul_ssid[0] && mesh.backhaul_key[0];

	if (bh_bss_on == (int)up)
		return;

	mesh_backhaul_bss_set(up);
	bh_bss_on = up;

	roam_log(ROAM_L_INFO, "mesh: backhaul network %s by this node", up ? "relayed" : "not relayed");
}

static bool bh_is_weak(void)
{
	return bh_weak_attach || (weak_since && roam_now - weak_since >= MESH_WEAK_HOLD);
}

static void bh_sample_link(void)
{
	char out[16];
	int signal;

	if (roam_now - link_sample_at < MESH_LINK_SAMPLE)
		return;

	link_sample_at = roam_now;

	if (!cmd_line(STA_IFACES "iw dev \"$i\" link 2>/dev/null | "
		      "sed -n 's/^[[:space:]]*signal: \\(-*[0-9]*\\).*/\\1/p'; done | head -1",
		      out, sizeof(out)))
		return;

	signal = atoi(out);

	if (bh_class_of(signal, true) != BH_WEAK) {
		weak_since = 0;
		bh_weak_attach = false;
	} else if (!weak_since) {
		weak_since = roam_now;
	}
}

static void bh_search(void)
{
	if (bh_scan_job.busy || (bh_scan_empty && roam_now - bh_scan_at < MESH_BH_SCAN_RETRY))
		return;

	bh_parents_load();
	bh_scan_at = roam_now;

	if (!bh_scan_start(false))
		bh_no_parent();
}

static void bh_scan_done(struct mesh_task *task, int ret)
{
	int best;

	roam_time_update();
	best = bh_scan_read() ? bh_select() : -1;

	if (bh_scan_rescan) {
		if (bh_up && best >= 0 && bh_class(&bh_parents[best]) != BH_WEAK &&
		    strcasecmp(bh_parents[best].bssid, bh_parent))
			bh_attach(best, "gives a stronger link");
		return;
	}

	if (bh_up || !mesh.backhaul_enabled || !contact_lost())
		return;

	if (best < 0) {
		bh_no_parent();
		return;
	}

	bh_attach(best, "chosen");
}

static void bh_weak_rescan(void)
{
	roam_time_update();

	if (!bh_up || !bh_is_weak() || contact_lost() || bh_scan_job.busy)
		return;

	if (rescan_at && roam_now - rescan_at < MESH_RESCAN_GAP)
		return;

	rescan_at = roam_now;
	bh_parents_load();
	bh_scan_start(true);
}

static struct uloop_fd probe = { .fd = -1 };
static struct in_addr probe_addr;
static uint16_t probe_seq;

static uint16_t icmp_sum(const uint8_t *p, size_t len)
{
	uint32_t sum = 0;

	for (; len > 1; p += 2, len -= 2)
		sum += (uint32_t)p[0] << 8 | p[1];

	if (len)
		sum += (uint32_t)p[0] << 8;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return htons((uint16_t)~sum);
}

static void probe_reply_cb(struct uloop_fd *u, unsigned int events)
{
	uint8_t buf[128];
	struct sockaddr_in from;
	socklen_t len = sizeof(from);
	ssize_t n;

	while ((n = recvfrom(u->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len)) > 0) {
		const struct iphdr *ip = (const struct iphdr *)buf;
		const struct icmphdr *ic;
		size_t hl = ip->ihl * 4;

		len = sizeof(from);
		if ((size_t)n < hl + sizeof(*ic) || from.sin_addr.s_addr != probe_addr.s_addr)
			continue;

		ic = (const struct icmphdr *)(buf + hl);
		if (ic->type == ICMP_ECHOREPLY && ic->un.echo.id == htons(MESH_PROBE_ID)) {
			roam_time_update();
			mesh_node_touch();
		}
	}
}

static void probe_send(void)
{
	struct sockaddr_in sa = { .sin_family = AF_INET };
	struct icmphdr ic = { .type = ICMP_ECHO };

	if (inet_pton(AF_INET, mesh.controller_addr, &sa.sin_addr) != 1)
		return;

	if (probe.fd < 0) {
		probe.fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
		if (probe.fd < 0)
			return;

		probe.cb = probe_reply_cb;
		uloop_fd_add(&probe, ULOOP_READ);
	}

	probe_addr = sa.sin_addr;
	ic.un.echo.id = htons(MESH_PROBE_ID);
	ic.un.echo.sequence = htons(++probe_seq);
	ic.checksum = icmp_sum((const uint8_t *)&ic, sizeof(ic));
	sendto(probe.fd, &ic, sizeof(ic), 0, (struct sockaddr *)&sa, sizeof(sa));
}

static bool uplink_alive(void)
{
	if (bh_up)
		return mesh_bridge_sta_up();

	if (uplink_port[0] && !mesh_bridge_carrier(uplink_port))
		return false;

	return !mesh_bridge_root_is_self(MESH_BRIDGE);
}

static void bh_linked(void)
{
	struct mesh_uplink up;

	bh_scan_empty = false;
	bh_want_search = false;

	if (cable_dropped_at && roam_now - cable_dropped_at >= MESH_CABLE_PROOF)
		cable_dropped_at = 0;

	if (!bh_up) {
		if (node_uplink(&up) && !up.wireless)
			snprintf(uplink_port, sizeof(uplink_port), "%s", up.ifname);
		return;
	}

	bh_sample_link();

	if (cable_penalty_until > roam_now || !node_wired_uplink()) {
		cable_since = 0;
		return;
	}

	if (!cable_since) {
		cable_since = roam_now;
		return;
	}

	if (roam_now - cable_since < MESH_CABLE_HOLD)
		return;

	roam_log(ROAM_L_INFO, "mesh: cable reaches the controller, backhaul station dropped");
	bh_detach(false);
	cable_dropped_at = roam_now;
}

static void bh_lost(bool alive)
{
	if (!mesh.backhaul_enabled)
		return;

	if (cable_dropped_at) {
		cable_dropped_at = 0;
		cable_penalty_until = roam_now + MESH_CABLE_PENALTY;
		bh_want_search = true;
		roam_log(ROAM_L_INFO, "mesh: cable lost the controller right after taking over, "
			 "staying on the air for 10 minutes");
	}

	if (bh_want_search) {
		bh_search();
		return;
	}

	if (bh_up && last_contact < bh_since) {
		if (!alive && roam_now - link_down_since >= MESH_LINK_GRACE_AIR) {
			bh_penalize("did not accept the station");
			bh_detach(true);
		} else if (alive && roam_now - bh_since >= MESH_PARENT_PROBATION) {
			bh_penalize("did not reach the controller");
			bh_detach(true);
		}
		return;
	}

	if (alive) {
		if (roam_now - last_contact < MESH_UPSTREAM_PATIENCE)
			return;

		if (bh_up) {
			bh_penalize("lost the controller");
			bh_detach(true);
			return;
		}

		bh_search();
		return;
	}

	if (roam_now - link_down_since < (bh_up ? MESH_LINK_GRACE_AIR : MESH_LINK_GRACE_CABLE))
		return;

	if (bh_up) {
		bh_detach(true);
		return;
	}

	bh_search();
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

static void node_watch(void)
{
	bool alive, relay;

	mesh_bridge_wifi_cost();

	probe_send();

	alive = uplink_alive();
	if (alive)
		link_down_since = 0;
	else if (!link_down_since)
		link_down_since = roam_now;

	if (!mesh.backhaul_enabled && bh_up)
		bh_detach(false);

	if (contact_lost())
		bh_lost(alive);
	else
		bh_linked();

	relay = !bh_no_path();
	link_led_set(!contact_lost());
	bh_bss_ensure(relay);

	if (mesh.wifi_shutdown)
		aps_ensure(relay);
	else if (aps_state_known && aps_down)
		aps_ensure(true);
}

static void watch_cb(struct uloop_timeout *t)
{
	roam_time_update();

	if (mesh.role == MESH_NODE)
		node_watch();
	else if (aps_state_known && aps_down)
		aps_ensure(true);

	uloop_timeout_set(&watch_timer, MESH_WATCH_INTERVAL);
}

static void bh_state_load(void)
{
	struct uci_session u;
	struct uci_section *sta;

	if (!uci_session_open(&u, "wireless"))
		return;

	sta = uci_lookup_section(u.ctx, u.pkg, MESH_BH_STA);

	if (sta && !roam_uci_bool(uci_lookup_option_string(u.ctx, sta, "disabled"))) {
		const char *device = uci_lookup_option_string(u.ctx, sta, "device");
		const char *bssid = uci_lookup_option_string(u.ctx, sta, "bssid");
		const char *radio = bh_radio(u.ctx, u.pkg, 1);

		snprintf(bh_parent, sizeof(bh_parent), "%s", bssid ? bssid : "");
		bh_up = true;
		bh_since = roam_now;
		bh_band = device && radio && !strcmp(device, radio);
	}

	uci_session_close(&u);

	if (bh_up)
		bh_owner_restore();
}

uint32_t mesh_node_contact_age(void)
{
	if (!last_contact || roam_now < last_contact)
		return 0;

	return (uint32_t)((roam_now - last_contact) / 1000);
}

void mesh_node_watch_start(void)
{
	last_contact = roam_now;
	bh_state_load();

	if (watch_timer.cb)
		return;

	watch_timer.cb = watch_cb;
	uloop_timeout_set(&watch_timer, MESH_WATCH_INTERVAL);
}

static void apply_table(struct uci_session *u, const char *section, struct blob_attr *table)
{
	struct blob_attr *cur;
	int rem;

	if (!table)
		return;

	blobmsg_for_each_attr(cur, table, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			continue;
		uci_session_set(u, section, blobmsg_name(cur), blobmsg_get_string(cur));
	}
}

enum {
	APPLY_CONTROLLER_ID,
	APPLY_GLOBAL,
	APPLY_POLICY,
	APPLY_BACKHAUL,
	APPLY_DEVICES,
	APPLY_WIFI,
	APPLY_SYSTEM,
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
	[APPLY_SYSTEM] = { .name = "system", .type = BLOBMSG_TYPE_TABLE },
	[APPLY_CREDS] = { .name = "credentials", .type = BLOBMSG_TYPE_TABLE },
};

enum { SYS_TIMEZONE, SYS_ZONENAME, __SYS_MAX };

static const struct blobmsg_policy system_policy[__SYS_MAX] = {
	[SYS_TIMEZONE] = { .name = "timezone", .type = BLOBMSG_TYPE_STRING },
	[SYS_ZONENAME] = { .name = "zonename", .type = BLOBMSG_TYPE_STRING },
};

static void apply_system(struct blob_attr *sys)
{
	static const char *const names[] = { "timezone", "zonename" };
	struct blob_attr *tb[__SYS_MAX];
	struct uci_session u;
	struct uci_section *sec;
	size_t i;

	if (!sys)
		return;

	blobmsg_parse(system_policy, __SYS_MAX, tb, blobmsg_data(sys), blobmsg_data_len(sys));

	if (!uci_session_open(&u, "system"))
		return;

	sec = uci_session_find(&u, "system", NULL, NULL);

	for (i = 0; sec && i < ARRAY_SIZE(names); i++) {
		const char *cur = uci_lookup_option_string(u.ctx, sec, names[i]);
		const char *want;

		if (!tb[i])
			continue;

		want = blobmsg_get_string(tb[i]);
		if (cur && !strcmp(cur, want))
			continue;

		uci_session_set(&u, sec->e.name, names[i], want);
	}

	if (u.dirty) {
		uci_session_close(&u);

		if (system("/etc/init.d/system reload >/dev/null 2>&1"))
			roam_log(ROAM_L_ERR, "mesh: time zone written, reload failed");
		else
			roam_log(ROAM_L_INFO, "mesh: time zone updated from the controller");

		return;
	}

	uci_session_close(&u);
}

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
	struct uci_session u;
	struct uci_element *e;
	bool changed;

	if (!wifi)
		return false;

	blobmsg_parse(wifi_policy, __WIFI_MAX, tb, blobmsg_data(wifi), blobmsg_data_len(wifi));
	if (!tb[WIFI_SSID])
		return false;

	if (!uci_session_open(&u, "wireless"))
		return false;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		size_t i;

		if (!strcmp(s->type, "wifi-device")) {
			if (roam_uci_bool(uci_lookup_option_string(u.ctx, s, "disabled")))
				uci_session_set(&u, s->e.name, "disabled", "0");
			continue;
		}

		if (!client_ap(u.ctx, s))
			continue;

		if (roam_uci_bool(uci_lookup_option_string(u.ctx, s, "disabled")))
			uci_session_set(&u, s->e.name, "disabled", "0");

		for (i = 0; i < __WIFI_MAX; i++)
			if (tb[i])
				uci_session_set(&u, s->e.name, names[i], blobmsg_get_string(tb[i]));
	}

	changed = u.dirty;
	uci_session_close(&u);

	return changed;
}

enum { DEV_MAC, DEV_BAND, DEV_NODES, __DEV_MAX };

static const struct blobmsg_policy dev_policy[__DEV_MAX] = {
	[DEV_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[DEV_BAND] = { .name = "band", .type = BLOBMSG_TYPE_STRING },
	[DEV_NODES] = { .name = "nodes", .type = BLOBMSG_TYPE_ARRAY },
};

static void apply_devices(struct uci_session *u, struct blob_attr *devices)
{
	struct uci_element *e, *tmp;
	struct blob_attr *cur;
	int rem;

	uci_foreach_element_safe(&u->pkg->sections, tmp, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "device"))
			uci_session_delete(u, s->e.name, NULL);
	}

	if (!devices)
		return;

	blobmsg_for_each_attr(cur, devices, rem) {
		struct blob_attr *tb[__DEV_MAX];
		struct uci_section *ns;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE)
			continue;

		blobmsg_parse(dev_policy, __DEV_MAX, tb,
			      blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[DEV_MAC])
			continue;

		ns = uci_session_add(u, "device", NULL);
		if (!ns)
			continue;

		uci_session_set(u, ns->e.name, "mac", blobmsg_get_string(tb[DEV_MAC]));

		if (tb[DEV_BAND])
			uci_session_set(u, ns->e.name, "band", blobmsg_get_string(tb[DEV_BAND]));

		if (tb[DEV_NODES]) {
			struct blob_attr *n;
			int nrem;

			blobmsg_for_each_attr(n, tb[DEV_NODES], nrem)
				if (blobmsg_type(n) == BLOBMSG_TYPE_STRING)
					uci_session_add_list(u, ns->e.name, "node", blobmsg_get_string(n));
		}
	}
}

bool mesh_node_apply(struct blob_attr *msg)
{
	struct blob_attr *tb[__APPLY_MAX];
	struct uci_session u;
	const char *from;

	blobmsg_parse(apply_policy, __APPLY_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[APPLY_CONTROLLER_ID])
		return false;

	from = blobmsg_get_string(tb[APPLY_CONTROLLER_ID]);
	if (mesh.controller_id[0] && strcmp(from, mesh.controller_id))
		return false;

	if (!uci_session_open(&u, "roamd"))
		return false;

	apply_table(&u, "global", tb[APPLY_GLOBAL]);
	apply_table(&u, "policy", tb[APPLY_POLICY]);
	apply_table(&u, "mesh", tb[APPLY_BACKHAUL]);
	apply_devices(&u, tb[APPLY_DEVICES]);

	uci_session_close(&u);

	if (apply_wifi(tb[APPLY_WIFI])) {
		uint32_t id;

		if (!ubus_lookup_id(ubus_ctx, "network", &id))
			ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
	}

	apply_system(tb[APPLY_SYSTEM]);
	apply_credentials(tb[APPLY_CREDS]);
	mesh_node_dumbap();

	mesh_node_touch();
	roam_config_load();
	roam_wireless_apply();
	roam_bss_recheck();

	if (bh_bss_on == 1)
		mesh_backhaul_bss_set(true);

	if (strcmp(bh_parents_seen, mesh.backhaul_parents)) {
		snprintf(bh_parents_seen, sizeof(bh_parents_seen), "%s", mesh.backhaul_parents);
		bh_weak_rescan();
	}

	return true;
}

static void node_profile(struct blob_buf *b)
{
	static const char *const fields[] = {
		"ssid", "encryption", "mobility_domain",
		"ieee80211r", "bss_transition", "ieee80211k"
	};
	struct uci_session u;
	struct uci_element *e;
	void *t;

	if (!uci_session_open(&u, "wireless"))
		return;

	t = blobmsg_open_table(b, "profile");

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		size_t i;

		if (!client_ap(u.ctx, s))
			continue;

		for (i = 0; i < ARRAY_SIZE(fields); i++) {
			const char *v = uci_lookup_option_string(u.ctx, s, fields[i]);

			if (v)
				blobmsg_add_string(b, fields[i], v);
		}
		break;
	}

	if (mesh.backhaul_ssid[0])
		blobmsg_add_string(b, "backhaul_ssid", mesh.backhaul_ssid);

	blobmsg_close_table(b, t);
	uci_session_close(&u);
}



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

static bool node_wired_uplink(void)
{
	struct mesh_uplink up;

	return node_uplink(&up) && !up.wireless;
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

static void node_identity_blob(struct blob_buf *b)
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
}

void mesh_node_diag(struct blob_buf *b)
{
	node_identity_blob(b);

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
	if (!sta || !sta->bss || !roam_policy_can_steer(sta))
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
	blobmsg_add_u8(&sb, "disassociation_imminent", 0);
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

	roam_log(ROAM_L_INFO, "mesh: asking %s to move to another node (attempt %u of %u)",
		 sta->mac, sta->steer_count, config.steer_retries);

	return true;
}

bool mesh_node_drop(const char *macstr)
{
	static struct blob_buf db;
	struct ether_addr *ea = ether_aton(macstr);
	struct roam_sta *sta;

	if (!ea)
		return false;

	sta = roam_sta_get(ea->ether_addr_octet, false);
	if (!sta || !sta->bss)
		return false;

	blob_buf_init(&db, 0);
	blobmsg_add_string(&db, "addr", sta->mac);
	blobmsg_add_u32(&db, "reason", MESH_DROP_REASON);
	blobmsg_add_u8(&db, "deauth", 1);
	roam_bss_invoke(sta->bss, "del_client", &db);

	roam_log(ROAM_L_INFO, "mesh: %s is associated elsewhere, removed from %s",
		 sta->mac, sta->bss->ifname);
	roam_sta_disconnected(sta);

	return true;
}

void mesh_self_info(struct blob_buf *b)
{
	struct sysinfo si;
	char value[32];

	node_identity_blob(b);

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
	void *clients;
	unsigned int count = 0;

	mesh_node_touch();
	mesh_assoc_collect(&assoc);
	if (mesh_parent_mac(pmac))
		mesh_wired_collect(&assoc, pmac);

	blobmsg_add_string(b, "role", mesh_role_name(mesh.role));
	blobmsg_add_string(b, "member_id", mesh.member_id);
	blobmsg_add_string(b, "controller_id", mesh.controller_id);

	node_identity_blob(b);
	blobmsg_add_string(b, "roamd_version", ROAMD_VERSION);

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
	mesh_log_dump(b, MESH_REPORT_EVENTS);
}
