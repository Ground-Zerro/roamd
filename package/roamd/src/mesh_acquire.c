#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define MEMBERS_DIR	"/etc/roamd/members"
#define TOKENS_DIR	"/etc/roamd/tokens"
#define SSH_SHORT	20000
#define SSH_LONG	300000
#define RESET_GONE_WAIT		90
#define RESET_BACK_WAIT		240
#define RESET_PROBE_TIMEOUT	10000
#define PAYLOAD_MAX	8192

static const char pkg_lock[] =
	"pkg_run() { ( exec 9>/tmp/roamd-pkg.lock; w=0; until flock -n 9; do "
	"[ \"$w\" -lt 120 ] || exit 1; sleep 2; w=$((w + 2)); done; \"$@\" ); }";

struct acquire_ctx {
	const char *task;
	char addr[MESH_ADDR_MAX];
	char mac[MESH_MAC_MAX];
	char member_id[MESH_ID_MAX];
	char hostname[MESH_NAME_MAX];
	char model[MESH_NAME_MAX];
	char version[MESH_WORD_MAX * 3];
	char branch[MESH_WORD_MAX];
	char arch[MESH_ARCH_MAX];
	char cidr[32];
	char caddr[32];
	char node_ip[32];
	char need[128];
};

static int fail(struct acquire_ctx *c, const char *step, const char *text)
{
	mesh_task_report(c->task, step, "error", text);

	return 1;
}

static bool addr_is_v6(const char *addr)
{
	return strchr(addr, ':') != NULL;
}

static void uplink_probe(const char *addr, char *out, size_t len)
{
	char bare[MESH_ADDR_MAX];
	const char *pct;

	snprintf(bare, sizeof(bare), "%s", addr);
	pct = strchr(bare, '%');
	if (pct)
		bare[pct - bare] = 0;

	if (addr_is_v6(addr))
		snprintf(out, len,
			 "ip -6 -o addr show scope link 2>/dev/null | "
			 "awk -v a='%s/' 'index($4, a) == 1 { print $2; exit }'", bare);
	else
		snprintf(out, len,
			 "ip -o -4 addr show 2>/dev/null | "
			 "awk -v a='%s/' 'index($4, a) == 1 { print $2; exit }'", bare);
}

static bool board_read(struct acquire_ctx *c)
{
	char buf[2048];
	struct blob_buf b = { 0 };
	struct blob_attr *cur;
	int rem;

	if (mesh_ssh(c->addr, "ubus call system board", buf, sizeof(buf), SSH_SHORT))
		return false;

	blob_buf_init(&b, 0);

	if (!blobmsg_add_json_from_string(&b, buf)) {
		blob_buf_free(&b);

		return false;
	}

	blob_for_each_attr(cur, b.head, rem) {
		const char *name = blobmsg_name(cur);

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
			if (!strcmp(name, "hostname"))
				snprintf(c->hostname, sizeof(c->hostname), "%s",
					 blobmsg_get_string(cur));
			else if (!strcmp(name, "model"))
				snprintf(c->model, sizeof(c->model), "%s",
					 blobmsg_get_string(cur));

			continue;
		}

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE || strcmp(name, "release"))
			continue;

		{
			struct blob_attr *f;
			int frem;

			blobmsg_for_each_attr(f, cur, frem)
				if (!strcmp(blobmsg_name(f), "version") &&
				    blobmsg_type(f) == BLOBMSG_TYPE_STRING)
					snprintf(c->version, sizeof(c->version), "%s",
						 blobmsg_get_string(f));
		}
	}

	blob_buf_free(&b);

	return c->version[0] != 0;
}

static bool conflict_check(struct acquire_ctx *c, char *out, size_t len)
{
	if (mesh_ssh(c->addr,
		     "for p in usteer dawn; do "
		     "(grep -qx \"Package: $p\" /usr/lib/opkg/status 2>/dev/null || "
		     "grep -qx \"P:$p\" /lib/apk/db/installed 2>/dev/null) && "
		     "{ echo $p; break; }; done", out, len, SSH_SHORT))
		return false;

	out[strcspn(out, "\r\n")] = 0;

	return out[0] != 0;
}

static bool mac_read(struct acquire_ctx *c)
{
	char probe[512], cmd[768], out[64], path[192];
	FILE *f;

	uplink_probe(c->addr, probe, sizeof(probe));
	snprintf(cmd, sizeof(cmd), "dev=$(%s); cat /sys/class/net/$dev/address 2>/dev/null",
		 probe);

	if (mesh_ssh(c->addr, cmd, out, sizeof(out), SSH_SHORT))
		return false;

	out[strcspn(out, "\r\n ")] = 0;
	if (strlen(out) < 17)
		return false;

	snprintf(c->mac, sizeof(c->mac), "%.17s", out);
	snprintf(c->member_id, sizeof(c->member_id), "%c%c%c%c%c%c",
		 out[9], out[10], out[12], out[13], out[15], out[16]);

	snprintf(path, sizeof(path), MESH_ACQUIRE_DIR "/%s.mac", c->task);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", c->mac);
		fclose(f);
	}

	return true;
}

static void linklocal_from_mac(const char *mac, const char *ifname, char *out, size_t len)
{
	unsigned int b[6];

	out[0] = '\0';

	if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return;

	snprintf(out, len, "fe80::%02x%02x:%02xff:fe%02x:%02x%02x%%%s",
		 b[0] ^ 0x02, b[1], b[2], b[3], b[4], b[5], ifname);
}

static bool factory_reset(struct acquire_ctx *c)
{
	char out[128], back[MESH_ADDR_MAX], lan[IFNAMSIZ];
	int waited;

	mesh_task_report(c->task, "reset", "progress",
			 "resetting the device to factory settings");

	if (mesh_ssh(c->addr,
		     "cat > /tmp/roamd-reset.sh <<'EOS'\n"
		     "#!/bin/sh\n"
		     "sleep 2\n"
		     "firstboot -y >/dev/null 2>&1\n"
		     "sync\n"
		     "reboot\n"
		     "EOS\n"
		     "ubus call service add '{\"name\":\"roamd-reset\",\"instances\":"
		     "{\"main\":{\"command\":[\"/bin/sh\",\"/tmp/roamd-reset.sh\"]}}}' "
		     ">/dev/null 2>&1\n"
		     "echo accepted\n", out, sizeof(out), SSH_SHORT) || !strstr(out, "accepted"))
		return false;

	for (waited = 0; waited < RESET_GONE_WAIT; waited += 5) {
		sleep(5);

		if (mesh_ssh(c->addr, "exit 0", NULL, 0, RESET_PROBE_TIMEOUT))
			break;
	}

	if (!mesh_bridge_lan(lan, sizeof(lan)))
		return false;

	linklocal_from_mac(c->mac, lan, back, sizeof(back));
	if (!back[0])
		return false;

	mesh_task_report(c->task, "reset", "progress",
			 "waiting for the device to come back after the reset");

	for (waited = 0; waited < RESET_BACK_WAIT; waited += 10) {
		sleep(10);

		if (mesh_ssh(back, "exit 0", NULL, 0, RESET_PROBE_TIMEOUT))
			continue;

		snprintf(c->addr, sizeof(c->addr), "%s", back);
		mesh_task_report(c->task, "reset", "ok",
				 "the device is reset to factory settings and back online");

		return true;
	}

	return false;
}

static bool deps_need(struct acquire_ctx *c)
{
	char out[256];

	if (mesh_ssh(c->addr, "roamd deps-ensure need; echo checked", out, sizeof(out), SSH_LONG))
		return false;

	if (!strstr(out, "checked"))
		return false;

	{
		char *end = strstr(out, "checked");
		size_t len = end ? (size_t)(end - out) : strlen(out);
		size_t i;

		snprintf(c->need, sizeof(c->need), "%.*s", (int)len, out);

		for (i = 0; c->need[i]; i++)
			if (c->need[i] == '\n' || c->need[i] == '\r')
				c->need[i] = ' ';

		while (i && c->need[i - 1] == ' ')
			c->need[--i] = 0;
	}

	return true;
}

static bool feeds_reachable(struct acquire_ctx *c, char *index, size_t len)
{
	if (mesh_ssh(c->addr,
		     "sed -nE 's|^src/gz [^[:space:]]+ (https?://[^[:space:]]+).*|\\1/Packages.gz|p; "
		     "s|^(https?://[^[:space:]]+/packages\\.adb)$|\\1|p' "
		     "/etc/opkg/distfeeds.conf /etc/apk/repositories.d/distfeeds.list 2>/dev/null | head -1",
		     index, len, SSH_SHORT))
		return false;

	index[strcspn(index, "\r\n ")] = 0;

	return index[0] != 0;
}

static int node_clock(struct acquire_ctx *c)
{
	char payload[160];
	time_t now = time(NULL);
	struct tm tm;

	gmtime_r(&now, &tm);
	snprintf(payload, sizeof(payload),
		 "date -u -s '%04d-%02d-%02d %02d:%02d:%02d' >/dev/null 2>&1\n"
		 "/etc/init.d/sysntpd restart >/dev/null 2>&1\n",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);

	return mesh_ssh(c->addr, payload, NULL, 0, SSH_SHORT);
}

static int enroll_role(struct acquire_ctx *c)
{
	char payload[PAYLOAD_MAX];
	char cname[MESH_NAME_MAX] = "";
	struct uci_session u;
	struct uci_section *s;

	if (uci_session_open(&u, "system")) {
		s = uci_session_find(&u, "system", NULL, NULL);
		if (s) {
			const char *v = uci_lookup_option_string(u.ctx, s, "hostname");

			if (v)
				snprintf(cname, sizeof(cname), "%s", v);
		}

		uci_session_close(&u);
	}

	snprintf(payload, sizeof(payload),
		 "uci -q get roamd.mesh >/dev/null || uci set roamd.mesh=mesh\n"
		 "for sect in $(uci show wireless 2>/dev/null | "
		 "sed -n 's/^wireless\\.\\(mesh_bh_[a-z0-9_]*\\)=wifi-iface$/\\1/p'); do "
		 "uci -q delete wireless.$sect; done\n"
		 "uci -q commit wireless\n"
		 "uci -q delete roamd.mesh.backhaul_ssid\n"
		 "uci -q delete roamd.mesh.backhaul_key\n"
		 "uci set roamd.mesh.role='node'\n"
		 "uci set roamd.mesh.controller_id='%s'\n"
		 "uci set roamd.mesh.controller_name='%s'\n"
		 "uci set roamd.mesh.controller_addr='%s'\n"
		 "uci set roamd.mesh.member_id='%s'\n"
		 "uci commit roamd\n"
		 "rm -f /tmp/luci-indexcache*\n"
		 "/etc/init.d/rpcd reload >/dev/null 2>&1\n"
		 "/etc/init.d/roamd enable >/dev/null 2>&1\n"
		 "/etc/init.d/roamd restart >/dev/null 2>&1\n",
		 mesh.controller_id, cname, c->caddr, c->member_id);

	return mesh_ssh(c->addr, payload, NULL, 0, SSH_LONG);
}

static int enroll_network(struct acquire_ctx *c)
{
	char payload[PAYLOAD_MAX], probe[512], netmask[32];

	if (!mesh_cidr_netmask(c->cidr, netmask, sizeof(netmask)))
		return 1;

	uplink_probe(c->addr, probe, sizeof(probe));

	snprintf(payload, sizeof(payload),
		 "uplink=$(%s)\n"
		 "[ -n \"$uplink\" ] || exit 1\n"
		 "br=''\n"
		 "for dev in $(uci show network | "
		 "sed -n 's/^network\\.\\(@device\\[[0-9]*\\]\\|[a-z0-9_]*\\)=device$/\\1/p'); do "
		 "[ \"$(uci -q get network.$dev.type)\" = 'bridge' ] || continue; "
		 "uci set network.$dev.stp='1'; [ -n \"$br\" ] || br=$dev; done\n"
		 "[ -n \"$br\" ] || exit 2\n"
		 "conduits=$(ip -o link 2>/dev/null | "
		 "sed -n 's/^[0-9]*: [^@]*@\\([^:]*\\):.*/\\1/p' | sort -u)\n"
		 "brname=$(uci -q get network.$br.name)\n"
		 "skip=0\n"
		 "[ \"$uplink\" = \"$brname\" ] && skip=1\n"
		 "for cd in $conduits; do [ \"$uplink\" = \"$cd\" ] && skip=1; done\n"
		 "case \" $(uci -q get network.$br.ports) \" in *\" $uplink \"*) skip=1 ;; esac\n"
		 "[ \"$skip\" = 1 ] || uci add_list network.$br.ports=\"$uplink\"\n"
		 "uci set network.$br.macaddr='%s'\n"
		 "uci set network.lan.proto='static'\n"
		 "uci set network.lan.ipaddr='%s'\n"
		 "uci set network.lan.netmask='%s'\n"
		 "uci set network.lan.gateway='%s'\n"
		 "uci -q delete network.lan.dns\n"
		 "uci add_list network.lan.dns='%s'\n"
		 "uci -q delete network.lan.ip6assign\n"
		 "uci -q delete network.wan\n"
		 "uci -q delete network.wan6\n"
		 "uci set dhcp.lan.ignore='1'\n"
		 "uci set dhcp.lan.dhcpv4='disabled'\n"
		 "uci set dhcp.lan.dhcpv6='disabled'\n"
		 "uci set dhcp.lan.ra='disabled'\n"
		 "uci set dhcp.@dnsmasq[0].noresolv='1'\n"
		 "uci -q delete dhcp.@dnsmasq[0].server\n"
		 "uci add_list dhcp.@dnsmasq[0].server='%s'\n"
		 "uci -q delete firewall.roamd_mesh\n"
		 "for sect in $(uci show firewall | "
		 "sed -n 's/^firewall\\.\\(@forwarding\\[[0-9]*\\]\\)=forwarding$/\\1/p' | "
		 "sed '1!G;h;$!d'); do uci delete firewall.$sect; done\n"
		 "for sect in $(uci show firewall | "
		 "sed -n 's/^firewall\\.\\(@nat\\[[0-9]*\\]\\|[a-z0-9_]*\\)=nat$/\\1/p' | "
		 "sed '1!G;h;$!d'); do uci delete firewall.$sect; done\n"
		 "for sect in $(uci show firewall | "
		 "sed -n 's/^firewall\\.\\(@zone\\[[0-9]*\\]\\|[a-z0-9_]*\\)=zone$/\\1/p' | "
		 "sed '1!G;h;$!d'); do "
		 "case \"$(uci -q get firewall.$sect.name)\" in "
		 "wan) uci delete firewall.$sect ;; "
		 "*) uci -q delete firewall.$sect.masq; uci -q delete firewall.$sect.masq6 ;; "
		 "esac; done\n"
		 "brname=$(uci -q get network.$br.name)\n"
		 "rm -rf /tmp/roamd-netrollback\n"
		 "mkdir -p /tmp/roamd-netrollback\n"
		 "cp /etc/config/network /etc/config/dhcp /etc/config/firewall "
		 "/tmp/roamd-netrollback/\n"
		 "uci commit network\nuci commit dhcp\nuci commit firewall\n"
		 "/etc/init.d/dnsmasq disable\n/etc/init.d/odhcpd disable\n"
		 "ubus call service add \"{\\\"name\\\":\\\"roamd-netapply\\\",\\\"instances\\\":"
		 "{\\\"main\\\":{\\\"command\\\":[\\\"/usr/sbin/roamd\\\",\\\"net-apply\\\","
		 "\\\"$brname\\\",\\\"%s\\\"]}}}\"\n",
		 probe, c->mac, c->node_ip, netmask, c->caddr, c->caddr, c->caddr, c->caddr);

	return mesh_ssh(c->addr, payload, NULL, 0, SSH_LONG);
}

static int enroll_channel(struct acquire_ctx *c, const char *token)
{
	char payload[PAYLOAD_MAX], px5g[MESH_WORD_MAX * 2] = "";
	const char *p = strstr(c->need, "px5g-");

	if (p) {
		size_t len = strcspn(p, " ");

		snprintf(px5g, sizeof(px5g), "%.*s", (int)len, p);
	}

	snprintf(payload, sizeof(payload),
		 "%s\n"
		 "if ! command -v px5g >/dev/null 2>&1 && [ -n '%s' ]; then "
		 "if command -v apk >/dev/null 2>&1; then "
		 "pkg_run apk update >/dev/null 2>&1; pkg_run apk add %s >/dev/null 2>&1; "
		 "else pkg_run opkg update >/dev/null 2>&1; "
		 "pkg_run opkg install %s >/dev/null 2>&1; fi; fi\n"
		 "command -v px5g >/dev/null 2>&1 || exit 3\n"
		 "px5g selfsigned -newkey ec -pkeyopt ec_paramgen_curve:P-256 -days 3650 "
		 "-keyout /etc/roamd-node.key -out /etc/roamd-node.crt -subj '/CN=%s' "
		 "-addext subjectAltName=DNS:%s >/dev/null 2>&1 || exit 3\n"
		 "uci set uhttpd.main.cert='/etc/roamd-node.crt'\n"
		 "uci set uhttpd.main.key='/etc/roamd-node.key'\n"
		 "uci commit uhttpd\n"
		 "h=$(echo '%s' | roamd crypt)\n"
		 "[ -n \"$h\" ] || exit 1\n"
		 "uci -q delete rpcd.roamd_mesh_login\n"
		 "uci set rpcd.roamd_mesh_login=login\n"
		 "uci set rpcd.roamd_mesh_login.username='mesh-%s'\n"
		 "uci set rpcd.roamd_mesh_login.password=\"$h\"\n"
		 "uci add_list rpcd.roamd_mesh_login.read='roamd'\n"
		 "uci add_list rpcd.roamd_mesh_login.write='roamd'\n"
		 "uci commit rpcd\n"
		 "/etc/init.d/rpcd reload >/dev/null 2>&1\n"
		 "/etc/init.d/uhttpd restart >/dev/null 2>&1\n",
		 pkg_lock, px5g, px5g, px5g, c->addr, c->addr, token, c->member_id);

	return mesh_ssh(c->addr, payload, NULL, 0, SSH_LONG);
}

static void enroll_key(struct acquire_ctx *c)
{
	char pubkey[MESH_PUBKEY_MAX], payload[PAYLOAD_MAX], cmd[MESH_PUBKEY_MAX * 3 + 256];
	bool installed = false;
	unsigned int k;

	for (k = 0; k < __MESH_KEY_MAX; k++) {
		if (!mesh_ssh_pubkey(k, pubkey, sizeof(pubkey)))
			continue;

		snprintf(cmd, sizeof(cmd),
			 "mkdir -p /etc/dropbear; grep -qxF '%s' /etc/dropbear/authorized_keys 2>/dev/null || "
			 "printf '%%s\\n' '%s' >> /etc/dropbear/authorized_keys; "
			 "chmod 600 /etc/dropbear/authorized_keys; "
			 "grep -qxF '%s' /etc/dropbear/authorized_keys", pubkey, pubkey, pubkey);

		if (!mesh_ssh(c->addr, cmd, NULL, 0, SSH_SHORT))
			installed = true;
	}

	if (!installed) {
		mesh_task_report(c->task, "enroll", "progress",
				 "service key was not installed, password login left enabled on the node");

		return;
	}

	snprintf(payload, sizeof(payload),
		 "rm -f /tmp/roamd-key-ok\n"
		 "uci -q set dropbear.@dropbear[0].RootPasswordAuth='off'\n"
		 "uci -q set dropbear.@dropbear[0].PasswordAuth='off'\n"
		 "uci -q commit dropbear\n"
		 "start-stop-daemon -S -b -x /bin/sh -- -c '"
		 "sleep 90; [ -f /tmp/roamd-key-ok ] && exit 0; "
		 "uci -q delete dropbear.@dropbear[0].RootPasswordAuth; "
		 "uci -q delete dropbear.@dropbear[0].PasswordAuth; "
		 "uci -q commit dropbear; /etc/init.d/dropbear restart' >/dev/null 2>&1\n"
		 "start-stop-daemon -S -b -x /bin/sh -- -c "
		 "'sleep 1; /etc/init.d/dropbear restart' >/dev/null 2>&1\n");

	mesh_ssh(c->addr, payload, NULL, 0, SSH_SHORT);
	sleep(12);

	if (!mesh_ssh(c->addr, "touch /tmp/roamd-key-ok", NULL, 0, SSH_SHORT))
		mesh_task_report(c->task, "enroll", "progress",
				 "service key works, password login disabled on the node");
	else
		mesh_task_report(c->task, "enroll", "progress",
				 "key login does not work, the node restores password login by itself in 90s");
}

int mesh_acquire_run(const char *addr, const char *task, bool reset)
{
	struct acquire_ctx c = { .task = task };
	char text[320], token[32], conflict[MESH_WORD_MAX];
	char state[128], index[256];
	int rc, waited;

	mesh_task_open(task);
	snprintf(c.addr, sizeof(c.addr), "%s", addr);

	mesh_ssh_key_ensure();
	mesh_node_forget(c.addr);

	if (!board_read(&c))
		return fail(&c, "probe", "SSH root without a password is not available");

	if (!mac_read(&c))
		return fail(&c, "probe", "cannot read the node MAC address");

	if (reset) {
		if (!factory_reset(&c))
			return fail(&c, "reset",
				    "the device did not come back after the factory reset");

		if (!board_read(&c))
			return fail(&c, "probe", "the device is not reachable after the reset");
	} else {
		mesh_task_report(task, "reset", "ok", "the reset is skipped by the user");
	}

	if (conflict_check(&c, conflict, sizeof(conflict))) {
		snprintf(text, sizeof(text),
			 "%.16s is part of the device firmware and conflicts with roamd — "
			 "it cannot be captured", conflict);

		return fail(&c, "probe", text);
	}

	snprintf(text, sizeof(text), "%.63s %.36s", c.hostname[0] ? c.hostname : "OpenWrt",
		 c.version);
	mesh_task_report(task, "probe", "ok", text);

	if (!mesh_node_release(c.addr, false, c.branch, sizeof(c.branch), c.arch,
			       sizeof(c.arch)))
		return fail(&c, "compat", "unknown OpenWrt version or architecture");

	if (!mesh_lan_cidr(c.cidr, sizeof(c.cidr)))
		return fail(&c, "compat", "the controller LAN has no private IPv4 subnet");

	snprintf(c.caddr, sizeof(c.caddr), "%.*s",
		 (int)strcspn(c.cidr, "/"), c.cidr);

	node_clock(&c);

	snprintf(text, sizeof(text), "installing roamd and interface for %s", c.branch);
	mesh_task_report(task, "install", "progress", text);

	rc = mesh_node_install(c.addr, c.branch, c.arch, true);
	switch (rc) {
	case 0:
		break;
	case MESH_NODE_FROM_CACHE:
		mesh_task_report(task, "install", "progress",
				 "repository unreachable, installed from the controller cache");
		break;
	case MESH_NODE_NO_PACKAGE:
		return fail(&c, "compat", "no package for this OpenWrt version and no feed access");
	case MESH_NODE_UNTRUSTED:
		return fail(&c, "install", "the package repository must be https (pkg_url)");
	default:
		return fail(&c, "install", "package installation failed on the node");
	}

	mesh_task_report(task, "install", "progress",
			 "checking what the node needs from the OpenWrt feeds");

	if (!deps_need(&c))
		return fail(&c, "install", "cannot ask the node which packages it needs");

	if (c.need[0]) {
		if (!feeds_reachable(&c, index, sizeof(index))) {
			snprintf(text, sizeof(text),
				 "the node needs %.60s but has no OpenWrt package feeds — capture "
				 "stopped before the node was changed", c.need);

			return fail(&c, "install", text);
		}

		if (!mesh_pkg_probe(index)) {
			snprintf(text, sizeof(text),
				 "the node needs %.50s from %.100s, which the controller cannot "
				 "reach — capture stopped", c.need, index);

			return fail(&c, "install", text);
		}
	}

	mesh_task_report(task, "enroll", "progress", "assigning node role");

	if (enroll_role(&c))
		return fail(&c, "enroll", "cannot assign the node role");

	mesh_task_report(task, "network", "progress", "switching the node to the controller subnet");

	if (!mesh_lease_reserve(c.cidr, c.mac, c.member_id, c.node_ip, sizeof(c.node_ip))) {
		snprintf(text, sizeof(text), "no free address for the node in the controller subnet %s",
			 c.cidr);

		return fail(&c, "network", text);
	}

	if (enroll_network(&c)) {
		mesh_lease_drop(c.member_id);

		return fail(&c, "network", "cannot switch the node to the controller subnet");
	}

	sleep(20);

	if (!mesh_node_wait_at(c.node_ip, c.mac, 180)) {
		mesh_lease_drop(c.member_id);
		snprintf(text, sizeof(text), "the node did not appear at %s in the controller subnet",
			 c.node_ip);

		return fail(&c, "network", text);
	}

	snprintf(c.addr, sizeof(c.addr), "%s", c.node_ip);

	{
		char payload[256];

		node_clock(&c);
		snprintf(payload, sizeof(payload),
			 "uci set roamd.mesh.controller_addr='%s'\n"
			 "uci commit roamd\n"
			 "/etc/init.d/roamd restart >/dev/null 2>&1\n", c.caddr);

		if (mesh_ssh(c.addr, payload, NULL, 0, SSH_LONG)) {
			snprintf(text, sizeof(text), "the node is unreachable at %s", c.addr);

			return fail(&c, "network", text);
		}
	}

	if (mesh_ssh(c.addr,
		     "printf 'dhcp-server=%s lan=%s nat=%s' "
		     "\"$(uci -q get dhcp.lan.ignore)\" \"$(uci -q get network.lan.proto)\" "
		     "\"$(nft list ruleset 2>/dev/null | grep -c masquerade)\"",
		     state, sizeof(state), SSH_SHORT))
		return fail(&c, "network", "cannot read the node network state");

	if (strcmp(state, "dhcp-server=1 lan=static nat=0")) {
		char rule[160] = "";

		mesh_ssh(c.addr, "nft list ruleset 2>/dev/null | grep -m1 masquerade", rule,
			 sizeof(rule), SSH_SHORT);
		rule[strcspn(rule, "\r\n")] = 0;

		snprintf(text, sizeof(text),
			 "the node at %.40s is still a router (%.60s%s%.80s) — single subnet",
			 c.addr, state, rule[0] ? ", " : "", rule);

		return fail(&c, "network", text);
	}

	mesh_task_report(task, "network", "ok", c.addr);
	mesh_task_report(task, "profile", "progress", "checking 802.11v support on the node");

	{
		char out[512], *line;

		rc = mesh_ssh(c.addr, "roamd deps-ensure", out, sizeof(out), SSH_LONG);
		line = strstr(out, "deps:");

		if (line) {
			line[strcspn(line, "\r\n")] = 0;
			mesh_task_report(task, "profile", "progress", line);
		}

		if (rc)
			return fail(&c, "profile",
				    "the node has no 802.11v-capable wpad and it cannot be installed");
	}

	mesh_task_report(task, "enroll", "progress", "securing the control channel");
	mkdir(MEMBERS_DIR, 0755);
	mkdir(TOKENS_DIR, 0700);

	if (!mesh_random_hex(token, 12))
		return fail(&c, "enroll", "cannot generate the control channel token");

	rc = enroll_channel(&c, token);
	if (rc == 3)
		return fail(&c, "enroll",
			    "px5g is not available on the node, the control channel cannot be secured");
	if (rc)
		return fail(&c, "enroll", "could not secure the control channel on the node");

	{
		char path[192];

		snprintf(path, sizeof(path), MEMBERS_DIR "/%s.crt", c.member_id);

		if (!mesh_scp_from(c.addr, "/etc/roamd-node.crt", path))
			return fail(&c, "enroll", "could not fetch the node certificate");

		snprintf(path, sizeof(path), TOKENS_DIR "/%s", c.member_id);
		{
			FILE *f = fopen(path, "w");

			if (!f)
				return fail(&c, "enroll", "cannot store the node token");

			fputs(token, f);
			fclose(f);
			chmod(path, 0600);
		}
	}

	mesh_task_report(task, "enroll", "progress", "installing service key");
	enroll_key(&c);

	{
		const char *label = c.hostname[0] ? c.hostname : "OpenWrt";

		if (!strcmp(label, "OpenWrt") && c.model[0])
			label = c.model;

		mesh_member_register(c.member_id, c.mac, c.addr,
				     c.hostname[0] ? c.hostname : "OpenWrt", label);
	}

	mesh_ubus_reload();
	mesh_task_report(task, "profile", "progress", "waiting for the node to broadcast the network");

	for (waited = 0; waited < 90; waited += 10) {
		char out[32];

		if (!mesh_ssh(c.addr, "ubus list | grep -c '^hostapd\\.phy'", out, sizeof(out),
			      SSH_SHORT)) {
			out[strcspn(out, "\r\n ")] = 0;

			if (out[0] && strcmp(out, "0")) {
				snprintf(text, sizeof(text),
					 "the node broadcasts the network on %s radio(s)", out);
				mesh_task_report(task, "profile", "ok", text);
				mesh_task_report(task, "done", "ok", c.member_id);

				return 0;
			}
		}

		sleep(10);
	}

	mesh_task_report(task, "profile", "error",
			 "the node has no access points up, check its wireless configuration");
	mesh_task_report(task, "done", "ok", c.member_id);

	return 0;
}
