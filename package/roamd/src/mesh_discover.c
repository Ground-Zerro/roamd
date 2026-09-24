#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define CANDIDATES	"/var/run/roamd/candidates.json"
#define NEIGH_MAX	256
#define PROBE_TIMEOUT	15000
#define PROBE_TRIES	3
#define SSH_PORT	22
#define PORT_TIMEOUT	1500

static const char port_closed[] = "SSH port is closed";

struct probe_info {
	char addr[MESH_ADDR_MAX];
	char mac[MESH_MAC_MAX];
	char name[MESH_NAME_MAX];
	char model[MESH_NAME_MAX];
	char board[MESH_NAME_MAX];
	char os[MESH_WORD_MAX * 3];
	char arch[MESH_ARCH_MAX];
	char conflict[MESH_WORD_MAX];
	const char *pkg;
};

static bool own_address(const char *addr, struct mesh_neigh *list, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (!strcmp(list[i].addr, addr))
			return true;

	return false;
}

static bool member_mac(const char *mac)
{
	struct mesh_member *m;

	list_for_each_entry(m, &mesh_members, list)
		if (!strcasecmp(m->mac, mac))
			return true;

	return false;
}

static bool board_parse(const char *json, struct probe_info *info)
{
	struct blob_buf b = { 0 };
	struct blob_attr *cur;
	bool ok = false;
	int rem;

	blob_buf_init(&b, 0);

	if (!blobmsg_add_json_from_string(&b, json)) {
		blob_buf_free(&b);

		return false;
	}

	blob_for_each_attr(cur, b.head, rem) {
		const char *name = blobmsg_name(cur);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
			if (!strcmp(name, "hostname"))
				snprintf(info->name, sizeof(info->name), "%.*s",
					 (int)sizeof(info->name) - 1, blobmsg_get_string(cur));
			else if (!strcmp(name, "model"))
				snprintf(info->model, sizeof(info->model), "%.*s",
					 (int)sizeof(info->model) - 1, blobmsg_get_string(cur));
			else if (!strcmp(name, "board_name"))
				snprintf(info->board, sizeof(info->board), "%.*s",
					 (int)sizeof(info->board) - 1, blobmsg_get_string(cur));

			continue;
		}

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE || strcmp(name, "release"))
			continue;

		{
			struct blob_attr *f;
			int frem;

			blobmsg_for_each_attr(f, cur, frem) {
				if (strcmp(blobmsg_name(f), "version") ||
				    blobmsg_type(f) != BLOBMSG_TYPE_STRING)
					continue;

				snprintf(info->os, sizeof(info->os), "%.*s",
					 (int)sizeof(info->os) - 1, blobmsg_get_string(f));
				ok = true;
			}
		}
	}

	blob_buf_free(&b);

	return ok || info->model[0];
}

static const char *pkg_state(const char *os, const char *arch)
{
	char branch[MESH_WORD_MAX], path[256];
	const char *dot;

	if (!os[0] || !arch[0])
		return "missing";

	dot = strrchr(os, '.');
	snprintf(branch, sizeof(branch), "%.*s",
		 dot ? (int)(dot - os) : (int)strlen(os), os);

	if (mesh_pkg_sync(branch, arch, MESH_PKG_MAIN))
		return "ready";

	return mesh_pkg_path(branch, arch, MESH_PKG_MAIN, path, sizeof(path)) ?
	       "ready" : "unreachable";
}

static const char *probe(const char *addr, const char *mac, struct probe_info *info)
{
	char buf[2048];
	unsigned int try;

	memset(info, 0, sizeof(*info));
	snprintf(info->addr, sizeof(info->addr), "%s", addr);
	snprintf(info->mac, sizeof(info->mac), "%s", mac);

	if (!mesh_port_open(addr, SSH_PORT, PORT_TIMEOUT))
		return port_closed;

	for (try = 0; mesh_ssh_pass(addr, "ubus call system board", buf, sizeof(buf),
				    PROBE_TIMEOUT); try++)
		if (try + 1 >= PROBE_TRIES)
			return "SSH root without a password is not available";

	if (!board_parse(buf, info))
		return "not an OpenWrt device";

	if (!mesh_ssh_pass(addr, "uci -q get roamd.mesh.role", buf, sizeof(buf), PROBE_TIMEOUT)) {
		buf[strcspn(buf, "\r\n")] = 0;

		if (!strcmp(buf, "node")) {
			char ctrl[MESH_ID_MAX + 2] = "";

			if (!mesh_ssh_pass(addr, "uci -q get roamd.mesh.controller_id", ctrl,
					   sizeof(ctrl), PROBE_TIMEOUT)) {
				ctrl[strcspn(ctrl, "\r\n")] = 0;

				if (ctrl[0] && strcmp(ctrl, mesh.controller_id))
					return "node of another controller";

				if (ctrl[0] && member_mac(mac))
					return "already a member";
			}
		}
	}

	if (!mesh_ssh_pass(addr,
			   "for p in usteer dawn; do "
			   "(grep -qx \"Package: $p\" /usr/lib/opkg/status 2>/dev/null || "
			   "grep -qx \"P:$p\" /lib/apk/db/installed 2>/dev/null) && "
			   "{ echo $p; break; }; done", buf, sizeof(buf), PROBE_TIMEOUT)) {
		buf[strcspn(buf, "\r\n")] = 0;
		snprintf(info->conflict, sizeof(info->conflict), "%.*s",
			 (int)sizeof(info->conflict) - 1, buf);
	}

	{
		char branch[MESH_WORD_MAX];

		if (!mesh_node_release(addr, true, branch, sizeof(branch), info->arch,
				       sizeof(info->arch)))
			info->arch[0] = 0;
	}

	info->pkg = pkg_state(info->os, info->arch);

	return NULL;
}

static bool mac_seen(const struct mesh_neigh *neigh, const bool *seen, unsigned int n,
		     const char *mac)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (seen[i] && !strcasecmp(neigh[i].mac, mac))
			return true;

	return false;
}

int mesh_discover_run(void)
{
	struct mesh_neigh neigh[NEIGH_MAX], own[8];
	bool seen[NEIGH_MAX] = { false };
	struct blob_buf b = { 0 };
	char lan[IFNAMSIZ], base[INET_ADDRSTRLEN] = "";
	unsigned int n, n_own = 0, i, hits = 0;
	char path[160], *json;
	void *arr;
	FILE *f;

	if (!mesh_bridge_lan(lan, sizeof(lan)))
		return 1;

	{
		struct uci_session u;
		struct uci_section *s;

		if (uci_session_open(&u, "network")) {
			s = uci_lookup_section(u.ctx, u.pkg, "lan");
			if (s) {
				const char *ip = uci_option_any(u.ctx, s, "ipaddr");
				char *slash;

				if (ip) {
					snprintf(base, sizeof(base), "%s", ip);
					slash = strchr(base, '/');
					if (slash)
						*slash = 0;

					snprintf(own[n_own].addr, sizeof(own[n_own].addr),
						 "%s", base);
					n_own++;

					slash = strrchr(base, '.');
					if (slash)
						*slash = 0;
				}
			}

			uci_session_close(&u);
		}
	}

	if (base[0])
		mesh_neigh_warm4(lan, base);

	mesh_neigh_warm6(lan);
	sleep(2);
	n = mesh_neigh_dump(lan, neigh, ARRAY_SIZE(neigh));
	roam_log(ROAM_L_DEBUG, "mesh: discover: %u neighbors on %s", n, lan);

	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "candidates");

	for (i = 0; i < n; i++) {
		struct probe_info info;
		char addr[MESH_ADDR_MAX];
		const char *skip;
		void *t;

		if (own_address(neigh[i].addr, own, n_own) ||
		    mac_seen(neigh, seen, i, neigh[i].mac))
			continue;

		if (neigh[i].v6)
			snprintf(addr, sizeof(addr), "%.45s%%%.15s", neigh[i].addr, lan);
		else
			snprintf(addr, sizeof(addr), "%.45s", neigh[i].addr);

		skip = probe(addr, neigh[i].mac, &info);
		seen[i] = skip != port_closed;

		if (skip) {
			roam_log(ROAM_L_DEBUG, "mesh: discover: %s (%s) skipped: %s",
				 addr, neigh[i].mac, skip);
			continue;
		}

		hits++;

		t = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "addr", info.addr);
		blobmsg_add_string(&b, "mac", info.mac);
		blobmsg_add_string(&b, "name", info.name[0] ? info.name : "OpenWrt");
		blobmsg_add_string(&b, "model", info.model);
		blobmsg_add_string(&b, "board", info.board);
		blobmsg_add_string(&b, "os", info.os);
		blobmsg_add_string(&b, "arch", info.arch);
		blobmsg_add_string(&b, "pkg", info.pkg ? info.pkg : "missing");

		if (info.conflict[0])
			blobmsg_add_string(&b, "conflict", info.conflict);

		blobmsg_close_table(&b, t);
	}

	blobmsg_close_array(&b, arr);
	roam_log(ROAM_L_INFO, "mesh: discover: %u candidates among %u neighbors on %s",
		 hits, n, lan);

	json = blobmsg_format_json(b.head, true);
	blob_buf_free(&b);

	if (!json)
		return 1;

	mesh_dir_ensure(NULL);
	snprintf(path, sizeof(path), CANDIDATES ".tmp");
	f = fopen(path, "w");
	if (f) {
		fputs(json, f);
		fclose(f);
		rename(path, CANDIDATES);
	}

	printf("%s\n", json);
	free(json);

	return 0;
}
