#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
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
#define MESH_RELEASE	"/usr/libexec/roamd/mesh-release.sh"
#define CANDIDATES	"/var/run/roamd/candidates.json"
#define ACQUIRE_DIR	"/var/run/roamd/acquire"
#define MEMBERS_DIR	"/var/run/roamd/members"
#define MESH_POLL_INTERVAL	15000
#define TASK_STALE	180

static struct uloop_process discover_proc;
static struct uloop_process acquire_proc;
static struct uloop_process poll_proc;
static struct uloop_process sync_proc;
static struct uloop_timeout poll_timer;
static bool discover_busy;
static bool poll_busy;
static bool sync_busy;
static char acquire_task[32];
static char acquire_addr[MESH_ADDR_MAX];
static const char *acquire_kind = "";

static void discover_done(struct uloop_process *p, int ret)
{
	discover_busy = false;
}

void mesh_ctrl_discover(struct blob_buf *b)
{
	char *data;

	if (!discover_busy) {
		pid_t pid = mesh_spawn(MESH_DISCOVER, NULL, NULL);

		if (pid > 0) {
			discover_proc.pid = pid;
			discover_proc.cb = discover_done;
			uloop_process_add(&discover_proc);
			discover_busy = true;
		}
	}

	blobmsg_add_u8(b, "scanning", discover_busy);

	data = mesh_slurp(CANDIDATES, 32768);
	if (data) {
		blobmsg_add_json_from_string(b, data);
		free(data);
	} else {
		void *a = blobmsg_open_array(b, "candidates");
		blobmsg_close_array(b, a);
	}
}

static bool acquire_busy(void)
{
	return acquire_proc.pid && !kill(acquire_proc.pid, 0);
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

	if (!acquire_busy() || !acquire_mac_read(mac, sizeof(mac)))
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

	blobmsg_add_u8(b, "running", acquire_busy());
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

static void acquire_done(struct uloop_process *p, int ret)
{
	roam_config_load();
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
	pid_t pid;

	if (acquire_busy()) {
		blobmsg_add_string(b, "error", "busy");
		blobmsg_add_string(b, "task_id", acquire_task);
		if (acquire_addr[0])
			blobmsg_add_string(b, "addr", acquire_addr);
		return;
	}

	snprintf(acquire_task, sizeof(acquire_task), "%llu", (unsigned long long)roam_now);
	snprintf(acquire_addr, sizeof(acquire_addr), "%s", addr ? addr : "");
	acquire_kind = kind;

	pid = mesh_spawn(script, arg ? arg : "", acquire_task);
	if (pid <= 0) {
		blobmsg_add_string(b, "error", "spawn_failed");
		return;
	}

	acquire_proc.pid = pid;
	acquire_proc.cb = acquire_done;
	uloop_process_add(&acquire_proc);

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
	bool running = acquire_busy() && !strcmp(task, acquire_task);
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

static void poll_done(struct uloop_process *p, int ret)
{
	poll_busy = false;
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

void mesh_uci_section(struct uci_context *ctx, struct uci_package *pkg)
{
	struct uci_ptr ptr = {
		.package = "roamd", .section = "mesh", .value = "mesh",
	};

	if (uci_lookup_section(ctx, pkg, "mesh"))
		return;

	uci_set(ctx, &ptr);
}

void mesh_ctrl_ensure_id(void)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;

	if (mesh.role != MESH_CONTROLLER || mesh.controller_id[0])
		return;

	gen_hex(mesh.controller_id, 8);

	ctx = uci_alloc_context();
	if (!ctx)
		return;

	if (uci_load(ctx, "roamd", &pkg) == UCI_OK) {
		mesh_uci_section(ctx, pkg);
		mesh_uci_set(ctx, "roamd", "mesh", "controller_id", mesh.controller_id);
		uci_commit(ctx, &pkg, false);
	}

	uci_free_context(ctx);
}

void mesh_ctrl_bridge_stp(void)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	uint32_t id;
	bool changed = false;

	if (mesh.role != MESH_CONTROLLER)
		return;

	ctx = uci_alloc_context();
	if (!ctx)
		return;

	if (uci_load(ctx, "network", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return;
	}

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *type = uci_lookup_option_string(ctx, s, "type");
		const char *stp = uci_lookup_option_string(ctx, s, "stp");
		struct uci_ptr ptr = {
			.package = "network", .section = s->e.name,
			.option = "stp", .value = "1",
		};

		if (strcmp(s->type, "device") || !type || strcmp(type, "bridge"))
			continue;
		if (stp && !strcmp(stp, "1"))
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

void mesh_ctrl_backhaul_apply(void)
{
	struct uci_context *ctx;
	struct uci_package *rp = NULL, *wp = NULL;
	struct uci_element *e;
	bool changed = false;

	if (mesh.role != MESH_CONTROLLER)
		return;

	ctx = uci_alloc_context();
	if (!ctx)
		return;

	if (uci_load(ctx, "roamd", &rp) != UCI_OK) {
		uci_free_context(ctx);
		return;
	}

	mesh_uci_section(ctx, rp);

	if (!mesh.backhaul_ssid[0]) {
		snprintf(mesh.backhaul_ssid, sizeof(mesh.backhaul_ssid),
			 "Service-Mesh-%.6s", mesh.controller_id);
		mesh_uci_set(ctx, "roamd", "mesh", "backhaul_ssid", mesh.backhaul_ssid);
		changed = true;
	}

	if (!mesh.backhaul_key[0]) {
		gen_hex(mesh.backhaul_key, 12);
		mesh_uci_set(ctx, "roamd", "mesh", "backhaul_key", mesh.backhaul_key);
		changed = true;
	}

	if (!mesh.ft_key[0]) {
		gen_hex(mesh.ft_key, 16);
		mesh_uci_set(ctx, "roamd", "mesh", "ft_key", mesh.ft_key);
		changed = true;
	}

	if (changed)
		uci_commit(ctx, &rp, false);

	if (uci_load(ctx, "wireless", &wp) != UCI_OK) {
		uci_free_context(ctx);
		return;
	}

	if (uci_lookup_section(ctx, wp, "mesh_backhaul")) {
		struct uci_ptr ptr = { .package = "wireless", .section = "mesh_backhaul" };

		if (uci_lookup_ptr(ctx, &ptr, NULL, false) == UCI_OK)
			uci_delete(ctx, &ptr);
	}

	uci_foreach_element(&wp->sections, e) {
		struct uci_section *s = uci_to_section(e);
		char name[MESH_NAME_MAX];

		if (strcmp(s->type, "wifi-device"))
			continue;

		snprintf(name, sizeof(name), "%s%s", MESH_BH_PREFIX, s->e.name);

		if (!uci_lookup_section(ctx, wp, name)) {
			struct uci_section *ns = NULL;

			uci_add_section(ctx, wp, "wifi-iface", &ns);
			if (!ns)
				continue;

			uci_rename(ctx, &(struct uci_ptr){
				.package = "wireless", .section = ns->e.name,
				.value = name });
		}

		mesh_uci_set(ctx, "wireless", name, "device", s->e.name);
		mesh_uci_set(ctx, "wireless", name, "mode", "ap");
		mesh_uci_set(ctx, "wireless", name, "network", "lan");
		mesh_uci_set(ctx, "wireless", name, "hidden", "1");
		mesh_uci_set(ctx, "wireless", name, "wds", "1");
		mesh_uci_set(ctx, "wireless", name, "encryption", "psk2");
		mesh_uci_set(ctx, "wireless", name, "ssid", mesh.backhaul_ssid);
		mesh_uci_set(ctx, "wireless", name, "key", mesh.backhaul_key);
		mesh_uci_set(ctx, "wireless", name, "disabled",
			    mesh.backhaul_enabled ? "0" : "1");
	}

	uci_commit(ctx, &wp, false);
	uci_free_context(ctx);
}

static void poll_cb(struct uloop_timeout *t)
{
	if (!poll_busy && !list_empty(&mesh_members)) {
		pid_t pid = mesh_spawn(MESH_POLL, "monitor", NULL);

		if (pid > 0) {
			poll_proc.pid = pid;
			poll_proc.cb = poll_done;
			uloop_process_add(&poll_proc);
			poll_busy = true;
		}
	}

	uloop_timeout_set(&poll_timer, MESH_POLL_INTERVAL);
}

static void sync_done(struct uloop_process *p, int ret)
{
	sync_busy = false;
}

void mesh_ctrl_sync(void)
{
	pid_t pid;

	if (mesh.role != MESH_CONTROLLER || sync_busy || list_empty(&mesh_members))
		return;

	pid = mesh_spawn(MESH_POLL, "force", NULL);
	if (pid <= 0)
		return;

	sync_proc.pid = pid;
	sync_proc.cb = sync_done;
	uloop_process_add(&sync_proc);
	sync_busy = true;
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
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	bool done = false;

	ctx = uci_alloc_context();
	if (!ctx)
		return false;

	if (uci_load(ctx, "roamd", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return false;
	}

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *sid;
		struct uci_ptr ptr = {
			.package = "roamd", .section = s->e.name,
			.option = "name", .value = name,
		};

		if (strcmp(s->type, "member"))
			continue;
		sid = uci_lookup_option_string(ctx, s, "id");
		if (sid && !strcmp(sid, id)) {
			uci_set(ctx, &ptr);
			uci_commit(ctx, &pkg, false);
			done = true;
			break;
		}
	}

	uci_free_context(ctx);

	if (done)
		roam_config_load();

	return done;
}

bool mesh_member_remove(const char *id)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	char *name = NULL;
	bool removed = false;

	ctx = uci_alloc_context();
	if (!ctx)
		return false;

	if (uci_load(ctx, "roamd", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return false;
	}

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *sid;

		if (strcmp(s->type, "member"))
			continue;
		sid = uci_lookup_option_string(ctx, s, "id");
		if (sid && !strcmp(sid, id)) {
			name = strdup(s->e.name);
			break;
		}
	}

	if (name) {
		struct uci_ptr ptr = { .package = "roamd", .section = name };

		if (uci_lookup_ptr(ctx, &ptr, NULL, false) == UCI_OK &&
		    uci_delete(ctx, &ptr) == UCI_OK) {
			char path[128];

			uci_commit(ctx, &pkg, false);
			snprintf(path, sizeof(path), "%s/%s.json", MEMBERS_DIR, id);
			unlink(path);
			snprintf(path, sizeof(path), "/etc/roamd/members/%s.crt", id);
			unlink(path);
			snprintf(path, sizeof(path), "/etc/roamd/tokens/%s", id);
			unlink(path);
			removed = true;
		}
		free(name);
	}

	uci_free_context(ctx);

	if (removed)
		roam_config_load();

	return removed;
}
