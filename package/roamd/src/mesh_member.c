#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include "roamd.h"
#include "mesh.h"

#define LEASES		"/tmp/dhcp.leases"
#define SSH_DIR		"/etc/roamd"
#define LEASE_START	100
#define LEASE_LIMIT	150
#define TAKEN_MAX	256
#define KEYGEN_TIMEOUT	120000

static uint32_t ip_num(const char *addr)
{
	struct in_addr in;

	if (!inet_pton(AF_INET, addr, &in))
		return 0;

	return ntohl(in.s_addr);
}

static void ip_text(uint32_t num, char *out, size_t len)
{
	struct in_addr in = { .s_addr = htonl(num) };

	inet_ntop(AF_INET, &in, out, len);
}

struct taken_list {
	char addr[TAKEN_MAX][INET_ADDRSTRLEN];
	unsigned int n;
};

static void taken_add(struct taken_list *t, const char *addr)
{
	unsigned int i;

	if (!addr || !addr[0] || t->n >= TAKEN_MAX)
		return;

	for (i = 0; i < t->n; i++)
		if (!strcmp(t->addr[i], addr))
			return;

	snprintf(t->addr[t->n++], INET_ADDRSTRLEN, "%s", addr);
}

static bool taken_has(const struct taken_list *t, const char *addr)
{
	unsigned int i;

	for (i = 0; i < t->n; i++)
		if (!strcmp(t->addr[i], addr))
			return true;

	return false;
}

static void taken_collect(struct taken_list *t, const char *lan)
{
	struct mesh_neigh neigh[128];
	struct uci_session u;
	struct uci_element *e;
	unsigned int n, i;
	char *data;

	data = mesh_slurp(LEASES, 16384);
	if (data) {
		char *line, *save = NULL;

		for (line = strtok_r(data, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char addr[INET_ADDRSTRLEN] = "";

			if (sscanf(line, "%*s %*s %15s", addr) == 1)
				taken_add(t, addr);
		}

		free(data);
	}

	if (uci_session_open(&u, "dhcp")) {
		uci_foreach_element(&u.pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);
			const char *ip = uci_lookup_option_string(u.ctx, s, "ip");

			if (ip)
				taken_add(t, ip);
		}

		uci_session_close(&u);
	}

	n = mesh_neigh_dump(lan, neigh, ARRAY_SIZE(neigh));
	for (i = 0; i < n; i++)
		if (!neigh[i].v6)
			taken_add(t, neigh[i].addr);
}

bool mesh_lan_cidr(char *out, size_t len)
{
	struct uci_session u;
	struct uci_section *s;
	const char *ip, *mask;
	char addr[INET_ADDRSTRLEN];
	bool ok = false;

	if (!uci_session_open(&u, "network"))
		return false;

	s = uci_lookup_section(u.ctx, u.pkg, "lan");
	ip = s ? uci_option_any(u.ctx, s, "ipaddr") : NULL;
	mask = s ? uci_option_any(u.ctx, s, "netmask") : NULL;

	if (ip && strchr(ip, '/')) {
		snprintf(out, len, "%s", ip);
		ok = true;
	} else if (ip) {
		uint32_t bits = 24, m = mask ? ip_num(mask) : 0;

		if (m) {
			bits = 0;
			while (m & 0x80000000u) {
				bits++;
				m <<= 1;
			}
		}

		snprintf(addr, sizeof(addr), "%s", ip);
		snprintf(out, len, "%s/%u", addr, bits);
		ok = true;
	}

	uci_session_close(&u);

	return ok;
}

bool mesh_cidr_netmask(const char *cidr, char *out, size_t len)
{
	const char *slash = strchr(cidr, '/');
	unsigned int bits = slash ? (unsigned int)atoi(slash + 1) : 24;

	if (bits > 32)
		return false;

	ip_text(bits ? ~0u << (32 - bits) : 0, out, len);

	return true;
}

bool mesh_lease_reserve(const char *cidr, const char *mac, const char *id,
			char *out, size_t len)
{
	struct taken_list taken = { 0 };
	struct uci_session u;
	char section[64], lan[IFNAMSIZ], base[INET_ADDRSTRLEN];
	uint32_t size, net, i;
	unsigned int bits;
	const char *slash;
	const char *have;
	bool ok = false;

	snprintf(section, sizeof(section), "roamd_%s", id);

	if (uci_session_open(&u, "dhcp")) {
		struct uci_section *s = uci_lookup_section(u.ctx, u.pkg, section);

		have = s ? uci_lookup_option_string(u.ctx, s, "ip") : NULL;
		if (have) {
			snprintf(out, len, "%s", have);
			ok = true;
		}

		uci_session_close(&u);
	}

	if (!ok) {
		slash = strchr(cidr, '/');
		bits = slash ? (unsigned int)atoi(slash + 1) : 24;
		snprintf(base, sizeof(base), "%.*s",
			 slash ? (int)(slash - cidr) : (int)strlen(cidr), cidr);

		size = bits >= 32 ? 1 : 1u << (32 - bits);
		net = ip_num(base) & (bits ? ~0u << (32 - bits) : 0);

		if (mesh_bridge_lan(lan, sizeof(lan))) {
			char prefix[INET_ADDRSTRLEN], *dot;

			snprintf(prefix, sizeof(prefix), "%s", base);
			dot = strrchr(prefix, '.');
			if (dot) {
				*dot = 0;
				mesh_neigh_warm(lan, prefix);
			}

			taken_collect(&taken, lan);
		}

		for (i = LEASE_START; i < LEASE_START + LEASE_LIMIT && i < size - 1; i++) {
			char addr[INET_ADDRSTRLEN];

			ip_text(net + i, addr, sizeof(addr));

			if (taken_has(&taken, addr))
				continue;

			snprintf(out, len, "%s", addr);
			ok = true;
			break;
		}
	}

	if (!ok)
		return false;

	if (!uci_session_open(&u, "dhcp"))
		return false;

	if (uci_session_add(&u, "host", section)) {
		uci_session_set(&u, section, "mac", mac);
		uci_session_set(&u, section, "ip", out);
	}

	uci_session_close(&u);
	mesh_dnsmasq_reload();

	return true;
}

void mesh_lease_drop(const char *id)
{
	struct uci_session u;
	char section[64];

	snprintf(section, sizeof(section), "roamd_%s", id);

	if (!uci_session_open(&u, "dhcp"))
		return;

	uci_session_delete(&u, section, NULL);
	uci_session_close(&u);
	mesh_dnsmasq_reload();
}

void mesh_dnsmasq_reload(void)
{
	char *argv[4] = { "/bin/sh", "-c", "/etc/init.d/dnsmasq reload >/dev/null 2>&1", NULL };

	mesh_run(argv, -1, NULL, 0, 20000);
}

const char *const mesh_key_files[__MESH_KEY_MAX] = {
	[MESH_KEY_ED25519] = SSH_DIR "/id",
	[MESH_KEY_RSA] = SSH_DIR "/id_rsa",
};

const char *const mesh_key_fields[__MESH_KEY_MAX] = {
	[MESH_KEY_ED25519] = "ssh_pubkey",
	[MESH_KEY_RSA] = "ssh_pubkey_rsa",
};

static const char *const key_types[__MESH_KEY_MAX] = {
	[MESH_KEY_ED25519] = "ed25519",
	[MESH_KEY_RSA] = "rsa",
};

bool mesh_ssh_key_ensure(void)
{
	bool ok = true;
	unsigned int k;

	mkdir(SSH_DIR, 0700);

	for (k = 0; k < __MESH_KEY_MAX; k++) {
		char *argv[] = { "/usr/bin/dropbearkey", "-t", (char *)key_types[k],
				 "-f", (char *)mesh_key_files[k], NULL };

		if (!access(mesh_key_files[k], R_OK))
			continue;

		roam_log(ROAM_L_INFO, "mesh: generating the %s service SSH key", key_types[k]);

		if (mesh_run(argv, -1, NULL, 0, KEYGEN_TIMEOUT)) {
			ok = false;
			continue;
		}

		chmod(mesh_key_files[k], 0600);
	}

	return ok;
}

bool mesh_ssh_pubkey(enum mesh_key key, char *out, size_t len)
{
	static struct {
		char text[MESH_PUBKEY_MAX];
		time_t mtime;
	} cache[__MESH_KEY_MAX];
	char *argv[] = { "/usr/bin/dropbearkey", "-y", "-f", (char *)mesh_key_files[key], NULL };
	char buf[1024], *start;
	struct stat st;

	if (stat(mesh_key_files[key], &st))
		return false;

	if (!cache[key].text[0] || cache[key].mtime != st.st_mtime) {
		if (mesh_run(argv, -1, buf, sizeof(buf), 20000))
			return false;

		start = strstr(buf, "ssh-");
		if (!start)
			return false;

		start[strcspn(start, "\r\n")] = 0;
		snprintf(cache[key].text, sizeof(cache[key].text), "%s", start);
		cache[key].mtime = st.st_mtime;
	}

	snprintf(out, len, "%s", cache[key].text);

	return out[0] != 0;
}

bool mesh_node_wait_at(const char *addr, const char *mac, int seconds)
{
	int waited = 0;

	while (waited < seconds) {
		char out[64];

		if (mesh_icmp_ping(addr, 2000) &&
		    !mesh_ssh(addr, "cat /sys/class/net/br-lan/address 2>/dev/null", out,
			      sizeof(out), 10000)) {
			out[strcspn(out, "\r\n")] = 0;

			if (!mac || !mac[0] || !strcasecmp(out, mac))
				return true;
		}

		sleep(5);
		waited += 5;
	}

	return false;
}

void mesh_member_register(const char *id, const char *mac, const char *addr,
			  const char *hostname, const char *label)
{
	struct uci_session u;
	struct uci_element *e;
	struct uci_section *found = NULL;
	char name[MESH_NAME_MAX + MESH_ID_MAX + 2];

	if (!uci_session_open(&u, "roamd"))
		return;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *smac = uci_lookup_option_string(u.ctx, s, "mac");

		if (!strcmp(s->type, "member") && smac && !strcasecmp(smac, mac))
			found = s;
	}

	if (!found)
		found = uci_session_add(&u, "member", NULL);

	if (!found) {
		uci_session_close(&u);

		return;
	}

	uci_session_set(&u, found->e.name, "id", id);
	uci_session_set(&u, found->e.name, "mac", mac);
	uci_session_set(&u, found->e.name, "addr", addr);
	uci_session_set(&u, found->e.name, "hostname", hostname);
	uci_session_set(&u, found->e.name, "managed", "1");

	if (!uci_lookup_option_string(u.ctx, found, "name")) {
		bool clash = false;

		uci_foreach_element(&u.pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);
			const char *sname = uci_lookup_option_string(u.ctx, s, "name");
			const char *sid = uci_lookup_option_string(u.ctx, s, "id");

			if (strcmp(s->type, "member") || !sname || !sid || !strcmp(sid, id))
				continue;

			if (!strcmp(sname, label))
				clash = true;
		}

		if (clash)
			snprintf(name, sizeof(name), "%s %s", label, id);
		else
			snprintf(name, sizeof(name), "%s", label);

		uci_session_set(&u, found->e.name, "name", name);
	}

	uci_session_close(&u);
}
