#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define NODE_SSH_SHORT	15000
#define NODE_SSH_LONG	240000
#define NODE_WAIT_STEP	5
#define NODE_WAIT_MAX	240

static const char pkg_lock[] =
	"pkg_run() { ( exec 9>/tmp/roamd-pkg.lock; w=0; until flock -n 9; do "
	"[ \"$w\" -lt 120 ] || exit 1; sleep 2; w=$((w + 2)); done; \"$@\" ); }";

bool mesh_node_release(const char *addr, bool pass, char *branch, size_t bl,
		       char *arch, size_t al)
{
	char buf[1024];
	int rc;

	rc = pass ? mesh_ssh_pass(addr, "cat /etc/openwrt_release", buf, sizeof(buf),
				  NODE_SSH_SHORT) :
		    mesh_ssh(addr, "cat /etc/openwrt_release", buf, sizeof(buf), NODE_SSH_SHORT);

	if (rc)
		return false;

	return mesh_release_parse(buf, branch, bl, arch, al);
}

static bool node_apk(const char *addr)
{
	return !mesh_ssh(addr, "command -v apk >/dev/null", NULL, 0, NODE_SSH_SHORT);
}

static bool node_has_luci(const char *addr)
{
	return !mesh_ssh(addr, "[ -d /www/luci-static ]", NULL, 0, NODE_SSH_SHORT);
}

static int pkg_push(const char *addr, const char *path, bool apk, bool force)
{
	char cmd[2048], install[192], remote[128], sum[65], out[128];
	const char *file = strrchr(path, '/');
	int waited = 0;

	file = file ? file + 1 : path;
	snprintf(remote, sizeof(remote), "/tmp/%s", file);

	if (!roam_sha256_file(path, sum, sizeof(sum)) || !mesh_scp(path, addr, remote))
		return 1;

	if (apk)
		snprintf(install, sizeof(install),
			 "apk add --allow-untrusted --repositories-file /dev/null '%s'", remote);
	else
		snprintf(install, sizeof(install), "opkg install%s '%s'",
			 force ? " --force-reinstall" : "", remote);

	snprintf(cmd, sizeof(cmd),
		 "echo '%s  %s' | sha256sum -c >/dev/null 2>&1 || { rm -f '%s'; exit 1; }; "
		 "rm -f /tmp/roamd-pkg.rc; "
		 "cat > /tmp/roamd-pkg.sh <<'EOS'\n"
		 "#!/bin/sh\n"
		 "%s\n"
		 "pkg_run %s </dev/null >>/tmp/roamd-install.log 2>&1\n"
		 "echo $? > /tmp/roamd-pkg.rc\n"
		 "rm -f '%s'\n"
		 "ubus call service delete '{\"name\":\"roamd-pkg\"}'\n"
		 "EOS\n"
		 "ubus call service add "
		 "'{\"name\":\"roamd-pkg\",\"instances\":{\"main\":{\"command\":"
		 "[\"/bin/sh\",\"/tmp/roamd-pkg.sh\"]}}}'",
		 sum, remote, remote, pkg_lock, install, remote);

	if (mesh_ssh(addr, cmd, NULL, 0, NODE_SSH_LONG))
		return 1;

	while (waited < NODE_WAIT_MAX) {
		sleep(NODE_WAIT_STEP);
		waited += NODE_WAIT_STEP;

		if (mesh_ssh(addr, "cat /tmp/roamd-pkg.rc 2>/dev/null", out, sizeof(out),
			     NODE_SSH_SHORT))
			continue;

		out[strcspn(out, "\r\n ")] = 0;
		if (!out[0])
			continue;

		mesh_ssh(addr, "rm -f /tmp/roamd-pkg.sh /tmp/roamd-pkg.rc", NULL, 0,
			 NODE_SSH_SHORT);

		return atoi(out);
	}

	return 1;
}

int mesh_node_install(const char *addr, const char *branch, const char *arch, bool force)
{
	static const char *const names[] = { MESH_PKG_MAIN, MESH_PKG_UI };
	char path[256];
	bool apk = node_apk(addr);
	bool luci = node_has_luci(addr);
	bool cached = false;
	size_t i;
	int try;

	mesh_ssh(addr, ": > /tmp/roamd-install.log", NULL, 0, NODE_SSH_SHORT);

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		bool main_pkg = !strcmp(names[i], MESH_PKG_MAIN);

		if (!main_pkg && !luci)
			continue;

		if (mesh_pkg_sync(branch, arch, names[i]) &&
		    mesh_pkg_path(branch, arch, names[i], path, sizeof(path))) {
			if (!pkg_push(addr, path, apk, force))
				continue;

			if (main_pkg)
				return MESH_NODE_INSTALL_FAILED;

			continue;
		}

		if (mesh_pkg_path(branch, arch, names[i], path, sizeof(path))) {
			cached = cached || main_pkg;

			if (!pkg_push(addr, path, apk, force))
				continue;

			if (main_pkg)
				return MESH_NODE_INSTALL_FAILED;

			continue;
		}

		if (!main_pkg)
			continue;

		{
			char cmd[512];

			snprintf(cmd, sizeof(cmd), "%s; pkg_run %s update >/dev/null 2>&1 && "
				 "pkg_run %s install roamd >/dev/null 2>&1", pkg_lock,
				 apk ? "apk" : "opkg", apk ? "apk add" : "opkg");

			if (mesh_ssh(addr, cmd, NULL, 0, NODE_SSH_LONG))
				return MESH_NODE_NO_PACKAGE;
		}
	}

	for (try = 0; try < 5; try++) {
		if (!mesh_ssh(addr, "command -v roamd >/dev/null", NULL, 0, NODE_SSH_SHORT))
			return cached ? MESH_NODE_FROM_CACHE : 0;

		sleep(5);
	}

	return MESH_NODE_INSTALL_FAILED;
}

int mesh_node_update(const char *addr, const char *id)
{
	char branch[MESH_WORD_MAX], arch[MESH_ARCH_MAX];
	int rc;

	mesh_node_forget(addr);

	if (!mesh_node_release(addr, false, branch, sizeof(branch), arch, sizeof(arch)))
		return MESH_NODE_UNREACHABLE;

	rc = mesh_node_install(addr, branch, arch, true);

	if (rc && rc != MESH_NODE_FROM_CACHE)
		return rc;

	if (id)
		mesh_member_pkg_source(id, rc == MESH_NODE_FROM_CACHE ? "cache" : "repository");

	mesh_ssh(addr, "/etc/init.d/roamd restart >/dev/null 2>&1", NULL, 0, NODE_SSH_SHORT);

	return rc;
}

const char *mesh_node_update_step(int rc)
{
	switch (rc) {
	case MESH_NODE_NO_PACKAGE:
		return "compat";
	case MESH_NODE_UNREACHABLE:
		return "probe";
	default:
		return "update";
	}
}

const char *mesh_node_update_error(int rc)
{
	switch (rc) {
	case MESH_NODE_NO_PACKAGE:
		return "no package for the OpenWrt version of the node";
	case MESH_NODE_UNTRUSTED:
		return "package source is not trusted: pkg_url must be https and "
		       "/etc/roamd/pkg.crt must exist";
	case MESH_NODE_UNREACHABLE:
		return "node is unreachable";
	default:
		return "package installation failed on the node";
	}
}

void mesh_member_pkg_source(const char *id, const char *source)
{
	char path[160];
	FILE *f;

	snprintf(path, sizeof(path), "/var/run/roamd/members/%s.pkgsrc", id);
	f = fopen(path, "w");
	if (!f)
		return;

	fputs(source, f);
	fclose(f);
}
