#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

#include <libubox/uloop.h>
#include <libubox/blobmsg_json.h>

#include <netinet/ether.h>

#include "roamd.h"
#include "mesh.h"

#define MESH_DISCOVER	"/usr/libexec/roamd/mesh-discover.sh"
#define MESH_ACQUIRE	"/usr/libexec/roamd/mesh-acquire.sh"
#define MESH_UPDATE	"/usr/libexec/roamd/mesh-update.sh"
#define MESH_SELFUPDATE	"/usr/libexec/roamd/mesh-selfupdate.sh"
#define MESH_POLL	"/usr/libexec/roamd/mesh-poll-all.sh"
#define MESH_AUTOUPDATE	"/usr/libexec/roamd/mesh-autoupdate.sh"
#define MESH_RELEASE	"/usr/libexec/roamd/mesh-release.sh"
#define CANDIDATES	"/var/run/roamd/candidates.json"
#define ACQUIRE_DIR	"/var/run/roamd/acquire"
#define MEMBERS_DIR	"/var/run/roamd/members"
#define MESH_POLL_INTERVAL	15000
#define TASK_STALE	180
#define AUTOUPDATE_SETTLE	60000
#define AUTOUPDATE_STEP		3600
#define AUTOUPDATE_BUSY_RETRY	300000
#define AUTOUPDATE_FAIL_RETRY	1800

static void discover_done(struct mesh_task *task, int ret);
static void acquire_done(struct mesh_task *task, int ret);
static void sync_done(struct mesh_task *task, int ret);

static bool sync_pending;
static bool discover_restart;

static struct mesh_task discover_job = { .done = discover_done };
static struct mesh_task acquire_job = { .done = acquire_done };
static struct mesh_task poll_job;
static struct mesh_task sync_job = { .done = sync_done };
static struct uloop_timeout poll_timer;
static struct uloop_timeout autoupdate_timer;
static char acquire_task[32];
static char acquire_addr[MESH_ADDR_MAX];
static const char *acquire_kind = "";

static void discover_start(void)
{
	unlink(CANDIDATES);
	mesh_task_start(&discover_job, MESH_DISCOVER, NULL, NULL);
}

static void discover_done(struct mesh_task *task, int ret)
{
	if (!discover_restart)
		return;

	discover_restart = false;
	discover_start();
}

void mesh_ctrl_discover(bool rescan, bool stop, struct blob_buf *b)
{
	char *data;

	if (stop) {
		discover_restart = false;
		mesh_task_stop(&discover_job);
	} else if (rescan && discover_job.busy) {
		discover_restart = true;
		mesh_task_stop(&discover_job);
	} else if (rescan) {
		discover_start();
	}

	blobmsg_add_u8(b, "scanning", discover_job.busy);

	data = mesh_slurp(CANDIDATES, 32768);
	if (data) {
		blobmsg_add_json_from_string(b, data);
		free(data);
	} else {
		void *a = blobmsg_open_array(b, "candidates");
		blobmsg_close_array(b, a);
	}
}

static bool acquire_mac_read(char *out, size_t size)
{
	char path[128], *data;

	if (!acquire_task[0])
		return false;

	snprintf(path, sizeof(path), "%s/%s.mac", ACQUIRE_DIR, acquire_task);
	data = mesh_slurp(path, 32);
	if (!data)
		return false;

	data[strcspn(data, "\r\n")] = '\0';
	snprintf(out, size, "%s", data);
	free(data);

	return out[0] != '\0';
}

bool mesh_ctrl_acquire_mac(uint8_t *out)
{
	struct ether_addr ea;
	char mac[32];

	if (!acquire_job.busy || !acquire_mac_read(mac, sizeof(mac)))
		return false;

	if (!ether_aton_r(mac, &ea))
		return false;

	memcpy(out, ea.ether_addr_octet, 6);

	return true;
}

void mesh_ctrl_acquire_blob(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "acquire");
	char mac[32];

	blobmsg_add_u8(b, "running", acquire_job.busy);
	if (acquire_kind[0])
		blobmsg_add_string(b, "kind", acquire_kind);
	if (acquire_task[0])
		blobmsg_add_string(b, "task_id", acquire_task);
	if (acquire_addr[0])
		blobmsg_add_string(b, "addr", acquire_addr);
	if (acquire_mac_read(mac, sizeof(mac)))
		blobmsg_add_string(b, "mac", mac);

	blobmsg_close_table(b, t);
}

static void acquire_done(struct mesh_task *task, int ret)
{
	roam_config_load();
	mesh_ctrl_backhaul_apply();
	mesh_ctrl_autoupdate_arm();
}

static struct mesh_member *member_by_id(const char *id)
{
	struct mesh_member *m;

	list_for_each_entry(m, &mesh_members, list)
		if (!strcmp(m->id, id))
			return m;

	return NULL;
}

static void acquire_start(const char *kind, const char *script, const char *arg,
			  const char *addr, struct blob_buf *b)
{
	if (acquire_job.busy) {
		if (b) {
			blobmsg_add_string(b, "error", "busy");
			blobmsg_add_string(b, "task_id", acquire_task);
			if (acquire_addr[0])
				blobmsg_add_string(b, "addr", acquire_addr);
		}
		return;
	}

	snprintf(acquire_task, sizeof(acquire_task), "%llu", (unsigned long long)roam_now);
	snprintf(acquire_addr, sizeof(acquire_addr), "%s", addr ? addr : "");
	acquire_kind = kind;

	if (!mesh_task_start(&acquire_job, script, arg ? arg : "", acquire_task)) {
		if (b)
			blobmsg_add_string(b, "error", "spawn_failed");
		return;
	}

	if (b)
		blobmsg_add_string(b, "task_id", acquire_task);
}

void mesh_ctrl_acquire(const char *addr, struct blob_buf *b)
{
	if (!addr || !addr[0]) {
		blobmsg_add_string(b, "error", "no_address");
		return;
	}

	acquire_start("acquire", MESH_ACQUIRE, addr, addr, b);
}

void mesh_ctrl_release(const char *id, struct blob_buf *b)
{
	struct mesh_member *target = member_by_id(id);

	if (!target) {
		blobmsg_add_string(b, "error", "not_found");
		return;
	}

	acquire_start("release", MESH_RELEASE, id, target->addr, b);
}

void mesh_ctrl_update(const char *id, struct blob_buf *b)
{
	struct mesh_member *target = member_by_id(id);

	if (!target) {
		blobmsg_add_string(b, "error", "not_found");
		return;
	}

	acquire_start("update", MESH_UPDATE, target->addr, target->addr, b);
}

void mesh_ctrl_self_update(struct blob_buf *b)
{
	acquire_start("selfupdate", MESH_SELFUPDATE, NULL, NULL, b);
}

void mesh_ctrl_acquire_status(const char *task, struct blob_buf *b)
{
	char path[128], *data, *line, *save;
	bool running = acquire_job.busy && !strcmp(task, acquire_task);
	bool terminal = false;
	struct stat st;
	void *steps;

	snprintf(path, sizeof(path), "%s/%s", ACQUIRE_DIR, task);
	data = mesh_slurp(path, 8192);
	if (!data) {
		if (running) {
			void *empty;

			blobmsg_add_u8(b, "running", 1);
			empty = blobmsg_open_array(b, "steps");
			blobmsg_close_array(b, empty);
			return;
		}

		blobmsg_add_string(b, "error", "no_such_task");
		return;
	}

	steps = blobmsg_open_array(b, "steps");
	for (line = strtok_r(data, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *status = strchr(line, '\t');
		char *message;
		void *e;

		if (!status)
			continue;
		*status++ = '\0';
		message = strchr(status, '\t');
		if (message)
			*message++ = '\0';

		e = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "step", line);
		blobmsg_add_string(b, "status", status);
		blobmsg_add_string(b, "message", message ? message : "");
		blobmsg_close_table(b, e);

		terminal = !strcmp(status, "error") ||
			   (!strcmp(line, "done") && !strcmp(status, "ok"));
	}
	blobmsg_close_array(b, steps);

	if (!running && !terminal && !stat(path, &st) &&
	    time(NULL) - st.st_mtime < TASK_STALE)
		running = true;

	blobmsg_add_u8(b, "running", running);

	free(data);
}

static void gen_hex(char *out, size_t bytes)
{
	unsigned char buf[32];
	int fd = open("/dev/urandom", O_RDONLY);
	size_t i;

	if (fd < 0 || read(fd, buf, bytes) != (ssize_t)bytes) {
		if (fd >= 0)
			close(fd);
		snprintf(out, bytes * 2 + 1, "%0*x", (int)(bytes * 2), (unsigned)roam_now);
		return;
	}
	close(fd);

	for (i = 0; i < bytes; i++)
		snprintf(out + i * 2, 3, "%02x", buf[i]);
}

void mesh_ctrl_ensure_id(void)
{
	struct uci_session u;

	if (mesh.role != MESH_CONTROLLER || mesh.controller_id[0])
		return;

	gen_hex(mesh.controller_id, 8);

	if (!uci_session_open(&u, "roamd"))
		return;

	uci_session_add(&u, "mesh", "mesh");
	uci_session_set(&u, "mesh", "controller_id", mesh.controller_id);
	uci_session_close(&u);
}

void mesh_ctrl_bridge_stp(void)
{
	struct uci_session u;
	struct uci_element *e;
	uint32_t id;

	if (mesh.role != MESH_CONTROLLER || list_empty(&mesh_members))
		return;

	if (!uci_session_open(&u, "network"))
		return;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *type = uci_lookup_option_string(u.ctx, s, "type");
		const char *stp = uci_lookup_option_string(u.ctx, s, "stp");

		if (strcmp(s->type, "device") || !type || strcmp(type, "bridge"))
			continue;
		if (stp && !strcmp(stp, "1"))
			continue;

		uci_session_set(&u, s->e.name, "stp", "1");
	}

	if (u.dirty && !ubus_lookup_id(ubus_ctx, "network", &id)) {
		uci_session_close(&u);
		ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
		return;
	}

	uci_session_close(&u);
}

void mesh_ctrl_backhaul_apply(void)
{
	bool want = mesh.backhaul_enabled && !list_empty(&mesh_members);
	struct uci_session u;
	struct uci_element *e;
	uint32_t id;

	if (mesh.role != MESH_CONTROLLER)
		return;

	if (!uci_session_open(&u, "roamd"))
		return;

	uci_session_add(&u, "mesh", "mesh");

	if (!mesh.ft_key[0]) {
		gen_hex(mesh.ft_key, 16);
		uci_session_set(&u, "mesh", "ft_key", mesh.ft_key);
	}

	if (want && !mesh.backhaul_ssid[0]) {
		snprintf(mesh.backhaul_ssid, sizeof(mesh.backhaul_ssid),
			 "Service-Mesh-%.6s", mesh.controller_id);
		uci_session_set(&u, "mesh", "backhaul_ssid", mesh.backhaul_ssid);
	}

	if (want && !mesh.backhaul_key[0]) {
		gen_hex(mesh.backhaul_key, 12);
		uci_session_set(&u, "mesh", "backhaul_key", mesh.backhaul_key);
	}

	uci_session_close(&u);

	if (!uci_session_open(&u, "wireless"))
		return;

	uci_session_delete(&u, "mesh_backhaul", NULL);

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		char name[MESH_NAME_MAX];

		if (strcmp(s->type, "wifi-device"))
			continue;

		snprintf(name, sizeof(name), "%s%s", MESH_BH_PREFIX, s->e.name);

		if (!uci_lookup_section(u.ctx, u.pkg, name) &&
		    (!want || !uci_session_add(&u, "wifi-iface", name)))
			continue;

		if (!want) {
			uci_session_set(&u, name, "disabled", "1");
			continue;
		}

		uci_session_set(&u, name, "device", s->e.name);
		uci_session_set(&u, name, "mode", "ap");
		uci_session_set(&u, name, "network", "lan");
		uci_session_set(&u, name, "hidden", "1");
		uci_session_set(&u, name, "wds", "1");
		uci_session_set(&u, name, "encryption", "psk2");
		uci_session_set(&u, name, "ssid", mesh.backhaul_ssid);
		uci_session_set(&u, name, "key", mesh.backhaul_key);
		uci_session_set(&u, name, "disabled", "0");
	}

	if (u.dirty && !ubus_lookup_id(ubus_ctx, "network", &id)) {
		uci_session_close(&u);
		ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
		return;
	}

	uci_session_close(&u);
}

static void poll_cb(struct uloop_timeout *t)
{
	if (!list_empty(&mesh_members))
		mesh_task_start(&poll_job, MESH_POLL, "monitor", NULL);

	uloop_timeout_set(&poll_timer, MESH_POLL_INTERVAL);
}

static const struct {
	const char *unit;
	uint32_t seconds;
	uint32_t max;
} autoupdate_units[] = {
	{ "hour", 3600, 23 },
	{ "day", 86400, 31 },
	{ "week", 604800, 5 },
	{ "month", 2592000, 12 },
};

static uint32_t autoupdate_interval(void)
{
	uint32_t every = mesh.auto_update_every ? mesh.auto_update_every : 1;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(autoupdate_units); i++) {
		if (strcmp(autoupdate_units[i].unit, mesh.auto_update_unit))
			continue;

		if (every > autoupdate_units[i].max)
			every = autoupdate_units[i].max;

		return every * autoupdate_units[i].seconds;
	}

	return autoupdate_units[1].seconds;
}

static uint32_t autoupdate_due(uint32_t now)
{
	uint32_t interval = autoupdate_interval();
	uint32_t wait = interval;

	if (!mesh.auto_update_last || mesh.auto_update_last > now)
		return now;

	if (!strcmp(mesh.auto_update_result, "error") && wait > AUTOUPDATE_FAIL_RETRY)
		wait = AUTOUPDATE_FAIL_RETRY;

	return mesh.auto_update_last + wait;
}

static void autoupdate_cb(struct uloop_timeout *t)
{
	uint32_t now = (uint32_t)time(NULL);
	uint32_t due = autoupdate_due(now);

	if (now < due) {
		uint32_t left = due - now;

		uloop_timeout_set(t, (int)(left > AUTOUPDATE_STEP ? AUTOUPDATE_STEP : left) * 1000);
		return;
	}

	if (acquire_job.busy) {
		uloop_timeout_set(t, AUTOUPDATE_BUSY_RETRY);
		return;
	}

	acquire_start("autoupdate", MESH_AUTOUPDATE, NULL, NULL, NULL);
	uloop_timeout_set(t, AUTOUPDATE_STEP * 1000);
}

void mesh_ctrl_autoupdate_arm(void)
{
	uloop_timeout_cancel(&autoupdate_timer);

	if (mesh.role != MESH_CONTROLLER || !mesh.enabled || !mesh.auto_update)
		return;

	autoupdate_timer.cb = autoupdate_cb;
	uloop_timeout_set(&autoupdate_timer, AUTOUPDATE_SETTLE);
}

static void sync_done(struct mesh_task *task, int ret)
{
	if (!sync_pending)
		return;

	sync_pending = false;
	mesh_ctrl_sync();
}

void mesh_ctrl_sync(void)
{
	if (mesh.role != MESH_CONTROLLER || list_empty(&mesh_members))
		return;

	if (!mesh_task_start(&sync_job, MESH_POLL, "force", NULL))
		sync_pending = true;
}

void mesh_ctrl_poll_start(void)
{
	if (poll_timer.cb)
		return;

	poll_timer.cb = poll_cb;
	uloop_timeout_set(&poll_timer, 2000);
}

void mesh_member_live(const char *id, struct blob_buf *b)
{
	char path[128], *data;

	snprintf(path, sizeof(path), "%s/%s.json", MEMBERS_DIR, id);
	data = mesh_slurp(path, MESH_REPORT_MAX);
	if (data && blobmsg_add_json_from_string(b, data)) {
		free(data);
		return;
	}

	if (data) {
		roam_log(ROAM_L_ERR, "mesh: unusable report of %s (%u bytes)",
			 id, (unsigned int)strlen(data));
		free(data);
	}

	blobmsg_add_u8(b, "online", 0);
}

bool mesh_member_rename(const char *id, const char *name)
{
	struct uci_session u;
	struct uci_section *sec;

	if (!uci_session_open(&u, "roamd"))
		return false;

	sec = uci_session_find(&u, "member", "id", id);
	if (sec)
		uci_session_set(&u, sec->e.name, "name", name);

	uci_session_close(&u);

	if (sec)
		roam_config_load();

	return sec != NULL;
}

bool mesh_member_remove(const char *id)
{
	struct uci_session u;
	struct uci_section *sec;
	bool removed = false;

	if (!uci_session_open(&u, "roamd"))
		return false;

	sec = uci_session_find(&u, "member", "id", id);
	if (sec && uci_session_delete(&u, sec->e.name, NULL)) {
		char path[128];

		snprintf(path, sizeof(path), "%s/%s.json", MEMBERS_DIR, id);
		unlink(path);
		snprintf(path, sizeof(path), "/etc/roamd/members/%s.crt", id);
		unlink(path);
		snprintf(path, sizeof(path), "/etc/roamd/tokens/%s", id);
		unlink(path);
		removed = true;
	}

	uci_session_close(&u);

	if (removed) {
		mesh_log_forget(id);
		roam_config_load();
		mesh_ctrl_backhaul_apply();
	}

	return removed;
}
