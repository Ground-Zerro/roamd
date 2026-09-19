#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define SELFCHECK	"/var/run/roamd/selfcheck.json"
#define ACQUIRE_DIR	"/var/run/roamd/acquire"
#define PKG_DIR		"/var/run/roamd/pkg"
#define OPKG_STATUS	"/usr/lib/opkg/status"
#define APK_STATUS	"/lib/apk/db/installed"
#define RELEASE_FILE	"/etc/openwrt_release"

struct self_job {
	char task[MESH_WORD_MAX * 2];
	char branch[MESH_WORD_MAX];
	char arch[MESH_ARCH_MAX];
	unsigned int pending;
	bool update;
	bool failed;
};

static struct self_job *self_job;

void mesh_task_report(const char *task, const char *step, const char *state, const char *text)
{
	char path[160];
	FILE *f;

	if (!task || !task[0])
		return;

	mesh_dir_ensure(ACQUIRE_DIR);
	snprintf(path, sizeof(path), ACQUIRE_DIR "/%s", task);

	f = fopen(path, "a");
	if (!f)
		return;

	fprintf(f, "%s\t%s\t%s\n", step, state, text ? text : "");
	fclose(f);
}

void mesh_task_open(const char *task)
{
	char path[160];
	FILE *f;

	if (!task || !task[0])
		return;

	mesh_dir_ensure(ACQUIRE_DIR);
	snprintf(path, sizeof(path), ACQUIRE_DIR "/%s", task);

	f = fopen(path, "w");
	if (f)
		fclose(f);
}

bool mesh_sys_apk(void)
{
	return !access(APK_STATUS, R_OK);
}

bool mesh_sys_installed(const char *name, char *out, size_t len)
{
	char *db = mesh_slurp(mesh_sys_apk() ? APK_STATUS : OPKG_STATUS, 512 * 1024);
	char *line, *save = NULL;
	bool want = false, found = false;

	if (!db)
		return false;

	for (line = strtok_r(db, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
		if (mesh_sys_apk()) {
			if (!strncmp(line, "P:", 2))
				want = !strcmp(line + 2, name);
			else if (want && !strncmp(line, "V:", 2)) {
				snprintf(out, len, "%s", line + 2);
				found = true;
			}

			continue;
		}

		if (!strncmp(line, "Package: ", 9))
			want = !strcmp(line + 9, name);
		else if (want && !strncmp(line, "Version: ", 9)) {
			snprintf(out, len, "%s", line + 9);
			found = true;
		}
	}

	free(db);

	return found;
}

bool mesh_release_parse(char *data, char *branch, size_t bl, char *arch, size_t al)
{
	char *line, *save = NULL;
	bool have_branch = false, have_arch = false;

	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char value[MESH_ARCH_MAX * 2], *dot;

		if (!strncmp(line, "DISTRIB_RELEASE=", 16)) {
			snprintf(value, sizeof(value), "%s", line + 16);
			mesh_unquote(value);
			dot = strrchr(value, '.');
			snprintf(branch, bl, "%.*s", dot ? (int)(dot - value) : (int)strlen(value),
				 value);
			have_branch = branch[0] != 0;
		} else if (!strncmp(line, "DISTRIB_ARCH=", 13)) {
			snprintf(value, sizeof(value), "%s", line + 13);
			mesh_unquote(value);
			snprintf(arch, al, "%s", value);
			have_arch = arch[0] != 0;
		}
	}

	return have_branch && have_arch;
}

bool mesh_self_release(char *branch, size_t bl, char *arch, size_t al)
{
	char *data = mesh_slurp(RELEASE_FILE, 4096);
	bool ok;

	if (!data)
		return false;

	ok = mesh_release_parse(data, branch, bl, arch, al);
	free(data);

	return ok;
}

static void selfcheck_write(struct self_job *job, bool index_ok)
{
	static const char *const names[] = { MESH_PKG_MAIN, MESH_PKG_UI };
	struct blob_buf b = { 0 };
	char path[128], *json;
	void *arr;
	size_t i;
	bool update = false;
	FILE *f;

	blob_buf_init(&b, 0);

	if (!index_ok) {
		blobmsg_add_string(&b, "error", "index_unreachable");
	} else {
		arr = blobmsg_open_array(&b, "packages");

		for (i = 0; i < ARRAY_SIZE(names); i++) {
			struct mesh_pkg_meta meta;
			char have[MESH_WORD_MAX * 3];
			bool outdated;
			void *t;

			if (!mesh_sys_installed(names[i], have, sizeof(have)))
				continue;

			if (!mesh_pkg_known(job->branch, job->arch, names[i], &meta))
				snprintf(meta.version, sizeof(meta.version), "%s", have);

			outdated = mesh_pkg_newer(have, meta.version) > 0;
			update |= outdated;

			t = blobmsg_open_table(&b, NULL);
			blobmsg_add_string(&b, "name", names[i]);
			blobmsg_add_string(&b, "installed", have);
			blobmsg_add_string(&b, "available", meta.version);
			blobmsg_add_u8(&b, "outdated", outdated);
			blobmsg_close_table(&b, t);
		}

		blobmsg_close_array(&b, arr);
		blobmsg_add_u8(&b, "update_available", update);
	}

	json = blobmsg_format_json(b.head, true);
	blob_buf_free(&b);

	if (!json)
		return;

	mesh_dir_ensure(NULL);
	snprintf(path, sizeof(path), SELFCHECK ".tmp");
	f = fopen(path, "w");
	if (f) {
		fputs(json, f);
		fclose(f);
		rename(path, SELFCHECK);
	}

	free(json);
}

static void selfcheck_ready(void *priv, bool ok)
{
	struct self_job *job = priv;

	if (!ok)
		job->failed = true;

	if (--job->pending)
		return;

	selfcheck_write(job, !job->failed);
	free(job);
	self_job = NULL;
}

bool mesh_self_check_busy(void)
{
	return self_job != NULL;
}

void mesh_self_check(void)
{
	static const char *const names[] = { MESH_PKG_MAIN, MESH_PKG_UI };
	struct self_job *job;
	size_t i;

	if (self_job)
		return;

	job = calloc(1, sizeof(*job));
	if (!job)
		return;

	if (!mesh_self_release(job->branch, sizeof(job->branch), job->arch, sizeof(job->arch))) {
		selfcheck_write(job, false);
		free(job);

		return;
	}

	self_job = job;
	job->pending = 1;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		char have[MESH_WORD_MAX * 3];

		if (!mesh_sys_installed(names[i], have, sizeof(have)))
			continue;

		job->pending++;

		if (!mesh_pkg_refresh(job->branch, job->arch, names[i], selfcheck_ready, job)) {
			job->pending--;
			job->failed = true;
		}
	}

	selfcheck_ready(job, true);
}

static void record_result(const char *result)
{
	struct uci_session u;
	char value[32];

	if (!uci_session_open(&u, "roamd"))
		return;

	snprintf(value, sizeof(value), "%llu", (unsigned long long)time(NULL));
	uci_session_set(&u, "mesh", "auto_update_last", value);
	uci_session_set(&u, "mesh", "auto_update_result", result);
	uci_session_close(&u);
}

static void daemon_restart(void)
{
	char *argv[4] = { "/bin/sh", "-c",
			  "( sleep 2; /etc/init.d/roamd restart ) >/dev/null 2>&1 &", NULL };

	mesh_run(argv, -1, NULL, 0, 5000);
}

static bool self_install(const char *branch, const char *arch, const char *name)
{
	char path[256], cmd[320];
	char *argv[8];
	unsigned int n = 0;

	if (!mesh_pkg_sync(branch, arch, name) ||
	    !mesh_pkg_path(branch, arch, name, path, sizeof(path)))
		return false;

	if (mesh_sys_apk())
		snprintf(cmd, sizeof(cmd),
			 "apk add --allow-untrusted --repositories-file /dev/null '%s'", path);
	else
		snprintf(cmd, sizeof(cmd), "opkg install --force-reinstall '%s'", path);

	argv[n++] = "/bin/sh";
	argv[n++] = "-c";
	argv[n++] = cmd;
	argv[n] = NULL;

	return mesh_run(argv, -1, NULL, 0, 240000) == 0;
}

static bool self_outdated(const char *branch, const char *arch, const char *name,
			  char *have, size_t len)
{
	struct mesh_pkg_meta meta;

	if (!mesh_sys_installed(name, have, len))
		return false;

	if (!mesh_pkg_sync(branch, arch, name) || !mesh_pkg_known(branch, arch, name, &meta))
		return false;

	return mesh_pkg_newer(have, meta.version) > 0;
}

int mesh_self_update(const char *task)
{
	static const char *const names[] = { MESH_PKG_MAIN, MESH_PKG_UI };
	char branch[MESH_WORD_MAX], arch[MESH_ARCH_MAX], have[MESH_WORD_MAX * 3];
	bool installed = false;
	size_t i;

	mesh_task_open(task);
	mesh_task_report(task, "update", "progress", "installing packages on the controller");

	if (!mesh_self_release(branch, sizeof(branch), arch, sizeof(arch))) {
		mesh_task_report(task, "update", "error", "package index is not available");

		return 1;
	}

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (!self_outdated(branch, arch, names[i], have, sizeof(have)))
			continue;

		if (!self_install(branch, arch, names[i])) {
			mesh_task_report(task, "update", "error", "package installation failed");

			return 1;
		}

		installed = true;
	}

	mesh_task_report(task, "done", "ok", installed ? "updated" : "no updates");

	if (installed)
		daemon_restart();

	return 0;
}

static bool member_needs_update(const char *id)
{
	char state[MESH_WORD_MAX];

	return mesh_member_state(id, "update_available", state, sizeof(state)) &&
	       !strcmp(state, "true");
}

int mesh_update_nodes(const char *task, bool updated)
{
	struct uci_session u;
	struct uci_element *e;
	bool failed = false;

	if (!uci_session_open(&u, "roamd"))
		return 1;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *id = uci_lookup_option_string(u.ctx, s, "id");
		const char *addr = uci_lookup_option_string(u.ctx, s, "addr");
		int rc;

		if (strcmp(s->type, "member") || !id || !addr || !member_needs_update(id))
			continue;

		mesh_task_report(task, "update", "progress", "updating roamd on the node");
		rc = mesh_node_update(addr, id);

		if (!rc) {
			updated = true;
		} else if (rc == MESH_NODE_FROM_CACHE) {
			updated = true;
			mesh_task_report(task, "update", "progress",
					 "repository unreachable, installed from the controller cache");
		} else {
			failed = true;
			mesh_task_report(task, mesh_node_update_step(rc), "error",
					 mesh_node_update_error(rc));
		}
	}

	uci_session_close(&u);

	if (updated) {
		record_result("updated");
		mesh_task_report(task, "done", "ok", "update installed");
	} else if (failed) {
		record_result("error");
		mesh_task_report(task, "done", "error", "some nodes were not updated");
	} else {
		record_result("none");
		mesh_task_report(task, "done", "ok", "no updates");
	}

	return failed && !updated ? 1 : 0;
}

int mesh_node_update_task(const char *addr, const char *task)
{
	char id[MESH_ID_MAX] = "";
	struct uci_session u;
	struct uci_element *e;
	int rc;

	mesh_task_open(task);
	mesh_task_report(task, "update", "progress", "updating roamd on the node");

	if (uci_session_open(&u, "roamd")) {
		uci_foreach_element(&u.pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);
			const char *sid = uci_lookup_option_string(u.ctx, s, "id");
			const char *saddr = uci_lookup_option_string(u.ctx, s, "addr");

			if (strcmp(s->type, "member") || !sid || !saddr || strcmp(saddr, addr))
				continue;

			snprintf(id, sizeof(id), "%s", sid);
			break;
		}

		uci_session_close(&u);
	}

	rc = mesh_node_update(addr, id[0] ? id : NULL);

	if (rc == MESH_NODE_FROM_CACHE)
		mesh_task_report(task, "update", "progress",
				 "repository unreachable, installed from the controller cache");
	else if (rc) {
		mesh_task_report(task, mesh_node_update_step(rc), "error",
				 mesh_node_update_error(rc));

		return 1;
	}

	mesh_task_report(task, "done", "ok", "updated");

	return 0;
}

int mesh_autoupdate_task(const char *phase, const char *task)
{
	char branch[MESH_WORD_MAX], arch[MESH_ARCH_MAX], have[MESH_WORD_MAX * 3];
	static const char *const names[] = { MESH_PKG_MAIN, MESH_PKG_UI };
	bool updated = false;
	size_t i;

	if (phase && !strcmp(phase, "nodes"))
		return mesh_update_nodes(task, true);

	mesh_task_open(task);
	mesh_task_report(task, "check", "progress", "checking the repository");

	if (!mesh_self_release(branch, sizeof(branch), arch, sizeof(arch))) {
		record_result("error");
		mesh_task_report(task, "check", "error", "package index is not available");

		return 1;
	}

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (!self_outdated(branch, arch, names[i], have, sizeof(have)))
			continue;

		if (!updated)
			mesh_task_report(task, "update", "progress",
					 "installing packages on the controller");

		if (!self_install(branch, arch, names[i])) {
			mesh_task_report(task, "update", "error", "package installation failed");

			return 1;
		}

		updated = true;
	}

	if (updated) {
		int rc;

		mesh_task_report(task, "update", "progress", "the controller is updated");
		rc = mesh_update_nodes(task, true);
		daemon_restart();

		return rc;
	}

	return mesh_update_nodes(task, false);
}
