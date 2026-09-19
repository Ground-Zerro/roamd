#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <libubus.h>
#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define DEPS_STATE	"/tmp/roamd/deps"
#define DEPS_TRIES	3
#define PKG_TIMEOUT	240000

static const char *const wpad_full[] = {
	"wpad", "wpad-openssl", "wpad-mbedtls", "wpad-wolfssl"
};

static const char *const wpad_any[] = {
	"wpad", "wpad-openssl", "wpad-mbedtls", "wpad-wolfssl",
	"wpad-basic", "wpad-basic-openssl", "wpad-basic-mbedtls", "wpad-basic-wolfssl",
	"wpad-mini", "wpad-mesh-openssl", "wpad-mesh-mbedtls", "wpad-mesh-wolfssl",
	"hostapd", "hostapd-openssl", "hostapd-mbedtls", "hostapd-wolfssl",
	"hostapd-basic", "hostapd-mini"
};

bool mesh_random_hex(char *out, size_t bytes)
{
	unsigned char buf[32];
	FILE *f;
	size_t i;

	if (bytes > sizeof(buf))
		return false;

	f = fopen("/dev/urandom", "r");
	if (!f)
		return false;

	if (fread(buf, 1, bytes, f) != bytes) {
		fclose(f);

		return false;
	}

	fclose(f);

	for (i = 0; i < bytes; i++)
		snprintf(out + i * 2, 3, "%02x", buf[i]);

	return true;
}

void mesh_ubus_reload(void)
{
	mesh_ubus_call("reload", NULL);
}

bool mesh_ubus_call(const char *method, const char *json)
{
	struct ubus_context *ctx = ubus_connect(NULL);
	static struct blob_buf b;
	uint32_t id;
	bool ok = false;

	if (!ctx)
		return false;

	if (!ubus_lookup_id(ctx, "roamd", &id)) {
		blob_buf_init(&b, 0);

		if (!json || blobmsg_add_json_from_string(&b, json))
			ok = !ubus_invoke(ctx, id, method, b.head, NULL, NULL, 5000);
	}

	ubus_free(ctx);

	return ok;
}

static const char *installed_one(const char *const *names, size_t n, char *version, size_t vlen)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (mesh_sys_installed(names[i], version, vlen))
			return names[i];

	return NULL;
}

static bool is_full(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(wpad_full); i++)
		if (!strcmp(name, wpad_full[i]))
			return true;

	return false;
}

static const char *full_variant(const char *name)
{
	if (strstr(name, "-openssl"))
		return "wpad-openssl";

	if (strstr(name, "-mbedtls"))
		return "wpad-mbedtls";

	if (strstr(name, "-wolfssl"))
		return "wpad-wolfssl";

	return "wpad";
}

static const char *px5g_variant(void)
{
	char version[MESH_WORD_MAX * 3];

	if (mesh_sys_installed("libustream-mbedtls", version, sizeof(version)))
		return "px5g-mbedtls";

	if (mesh_sys_installed("libustream-wolfssl", version, sizeof(version)))
		return "px5g-wolfssl";

	return "px5g-standalone";
}

static bool px5g_present(void)
{
	return !access("/usr/sbin/px5g", X_OK) || !access("/usr/bin/px5g", X_OK);
}

static void deps_state(const char *text)
{
	FILE *f;

	mkdir("/tmp/roamd", 0755);
	f = fopen(DEPS_STATE, "w");
	if (f) {
		fprintf(f, "%s\n", text);
		fclose(f);
	}

	printf("deps: %s\n", text);
	roam_log(ROAM_L_INFO, "deps: %s", text);
}

static bool pkg_cmd(const char *action, const char *name)
{
	char cmd[256];
	char *argv[4] = { "/bin/sh", "-c", cmd, NULL };

	snprintf(cmd, sizeof(cmd),
		 "pkg_run() { ( exec 9>/tmp/roamd-pkg.lock; w=0; until flock -n 9; do "
		 "[ \"$w\" -lt 120 ] || exit 1; sleep 2; w=$((w + 2)); done; \"$@\" ); }; "
		 "pkg_run %s %s >/dev/null 2>&1", action, name ? name : "");

	return mesh_run(argv, -1, NULL, 0, PKG_TIMEOUT) == 0;
}

static bool have_network(void)
{
	char *argv[4] = { "/bin/sh", "-c",
			  "wget -q -s -T 4 http://downloads.openwrt.org/ >/dev/null 2>&1 || "
			  "wget -q -s -T 4 http://openwrt.org/ >/dev/null 2>&1", NULL };

	return mesh_run(argv, -1, NULL, 0, 20000) == 0;
}

static void wpad_reload(void)
{
	char *argv[4] = { "/bin/sh", "-c",
			  "[ -x /etc/init.d/wpad ] && /etc/init.d/wpad restart >/dev/null 2>&1; "
			  "sleep 3; wifi down >/dev/null 2>&1; sleep 2; wifi up >/dev/null 2>&1",
			  NULL };

	mesh_run(argv, -1, NULL, 0, 60000);
}

int mesh_deps_ensure(bool need_only)
{
	char version[MESH_WORD_MAX * 3], text[160];
	const char *cur, *want;
	bool apk = mesh_sys_apk();

	cur = installed_one(wpad_any, ARRAY_SIZE(wpad_any), version, sizeof(version));

	if (need_only) {
		if (cur && !is_full(cur))
			printf("%s\n", full_variant(cur));

		if (!px5g_present())
			printf("%s\n", px5g_variant());

		return 0;
	}

	if (!cur) {
		deps_state("no hostapd package installed");

		return 1;
	}

	if (is_full(cur)) {
		snprintf(text, sizeof(text), "%s supports 802.11v", cur);
		deps_state(text);

		return 0;
	}

	want = full_variant(cur);

	if (!have_network()) {
		deps_state("no network yet, dependencies will be checked again later");

		return 1;
	}

	if (!pkg_cmd(apk ? "apk update" : "opkg update", NULL)) {
		snprintf(text, sizeof(text),
			 "package index is not available, %s kept — 802.11v unavailable", cur);
		deps_state(text);

		return 1;
	}

	if (apk) {
		pkg_cmd("apk del", cur);

		if (!pkg_cmd("apk add", want))
			pkg_cmd("apk add", cur);
	} else {
		pkg_cmd("opkg remove --force-depends", cur);

		if (!pkg_cmd("opkg install", want))
			pkg_cmd("opkg install", cur);
	}

	cur = installed_one(wpad_any, ARRAY_SIZE(wpad_any), version, sizeof(version));

	if (!cur || !is_full(cur)) {
		snprintf(text, sizeof(text), "cannot replace with %s — 802.11v unavailable", want);
		deps_state(text);

		return 1;
	}

	wpad_reload();
	snprintf(text, sizeof(text), "%s supports 802.11v", cur);
	deps_state(text);

	return 0;
}

int mesh_release_run(const char *id, const char *task)
{
	char addr[MESH_ADDR_MAX] = "", args[96], out[128];
	struct uci_session u;
	struct uci_element *e;
	int waited;

	mesh_task_open(task);

	if (uci_session_open(&u, "roamd")) {
		uci_foreach_element(&u.pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);
			const char *sid = uci_lookup_option_string(u.ctx, s, "id");
			const char *saddr = uci_lookup_option_string(u.ctx, s, "addr");

			if (strcmp(s->type, "member") || !sid || !saddr || strcmp(sid, id))
				continue;

			snprintf(addr, sizeof(addr), "%s", saddr);
			break;
		}

		uci_session_close(&u);
	}

	snprintf(args, sizeof(args), "{\"id\":\"%s\"}", id);

	if (!addr[0]) {
		mesh_task_report(task, "reset", "error", "the node is not in the system");
		mesh_task_report(task, "done", "ok", id);

		return 0;
	}

	mesh_task_report(task, "reset", "progress", "resetting the node to factory settings");

	if (mesh_ssh(addr,
		     "cat > /tmp/roamd-release.sh <<'EOS'\n"
		     "#!/bin/sh\n"
		     "sleep 2\n"
		     "firstboot -y >/dev/null 2>&1\n"
		     "sync\n"
		     "reboot\n"
		     "EOS\n"
		     "ubus call service add '{\"name\":\"roamd-release\",\"instances\":"
		     "{\"main\":{\"command\":[\"/bin/sh\",\"/tmp/roamd-release.sh\"]}}}' "
		     ">/dev/null 2>&1\n"
		     "echo accepted\n", out, sizeof(out), 30000) || !strstr(out, "accepted")) {
		mesh_task_report(task, "reset", "error",
				 "the node did not accept the reset, release it manually");
		mesh_ubus_call("mesh_member_remove", args);
		mesh_node_forget(addr);
		mesh_task_report(task, "done", "ok", id);

		return 0;
	}

	for (waited = 0; waited < 90; waited += 5) {
		sleep(5);

		if (mesh_ssh(addr, "exit 0", NULL, 0, 10000)) {
			mesh_task_report(task, "reset", "ok",
					 "the node is resetting to factory settings");
			mesh_ubus_call("mesh_member_remove", args);
			mesh_node_forget(addr);
			mesh_task_report(task, "done", "ok", id);

			return 0;
		}
	}

	mesh_task_report(task, "reset", "error",
			 "the node is still reachable, the reset may have failed");
	mesh_ubus_call("mesh_member_remove", args);
	mesh_node_forget(addr);
	mesh_task_report(task, "done", "ok", id);

	return 0;
}
