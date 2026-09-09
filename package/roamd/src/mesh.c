#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/ether.h>
#include <libubox/uloop.h>

#include "roamd.h"
#include "mesh.h"

#define MESH_UBUS_TIMEOUT	500
#define MESH_DEPS_FIRST		5000
#define MESH_DEPS_RETRY		300000

static struct uloop_process deps_proc;
static struct uloop_timeout deps_timer;
static bool deps_busy;
static bool deps_ready;

pid_t mesh_spawn(const char *script, const char *arg1, const char *arg2)
{
	pid_t pid = fork();

	if (pid == 0) {
		int fd = open("/dev/null", O_WRONLY);

		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		execl("/bin/sh", "sh", script, arg1, arg2, (char *)NULL);
		_exit(127);
	}

	return pid;
}

static void deps_done(struct uloop_process *p, int ret)
{
	deps_busy = false;
	deps_ready = ret == 0;

	if (!deps_ready)
		uloop_timeout_set(&deps_timer, MESH_DEPS_RETRY);
}

static void deps_cb(struct uloop_timeout *t)
{
	if (deps_busy)
		return;

	deps_proc.pid = mesh_spawn(MESH_DEPS_SCRIPT, NULL, NULL);
	if (deps_proc.pid <= 0) {
		uloop_timeout_set(&deps_timer, MESH_DEPS_RETRY);
		return;
	}

	deps_busy = true;
	deps_proc.cb = deps_done;
	uloop_process_add(&deps_proc);
}

void mesh_deps_start(void)
{
	if (deps_timer.cb)
		return;

	deps_timer.cb = deps_cb;
	uloop_timeout_set(&deps_timer, MESH_DEPS_FIRST);
}

void mesh_deps_blob(struct blob_buf *b)
{
	char *state = mesh_slurp(MESH_DEPS_STATE, 256);

	blobmsg_add_u8(b, "deps_ready", deps_ready);

	if (state) {
		state[strcspn(state, "\r\n")] = '\0';
		if (state[0])
			blobmsg_add_string(b, "deps_state", state);
		free(state);
	}
}

char *mesh_slurp(const char *path, size_t max)
{
	char *buf;
	int fd, n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	buf = malloc(max + 1);
	if (!buf) {
		close(fd);
		return NULL;
	}

	n = read(fd, buf, max);
	close(fd);
	if (n < 0) {
		free(buf);
		return NULL;
	}

	buf[n] = '\0';
	return buf;
}

struct mesh_config mesh;
struct list_head mesh_members = LIST_HEAD_INIT(mesh_members);

enum mesh_field_type {
	MESH_BOOL,
	MESH_ROLE,
	MESH_STR
};

struct mesh_field {
	const char *name;
	enum mesh_field_type type;
	size_t offset;
	size_t size;
};

#define MFIELD(n, t, f) { n, t, offsetof(struct mesh_config, f), sizeof(((struct mesh_config *)0)->f) }

static const struct mesh_field fields[] = {
	MFIELD("enabled", MESH_BOOL, enabled),
	MFIELD("role", MESH_ROLE, role),
	MFIELD("backhaul_enabled", MESH_BOOL, backhaul_enabled),
	MFIELD("backhaul_ssid", MESH_STR, backhaul_ssid),
	MFIELD("backhaul_key", MESH_STR, backhaul_key),
	MFIELD("backhaul_bssids", MESH_STR, backhaul_bssids),
	MFIELD("ft_key", MESH_STR, ft_key),
	MFIELD("wifi_shutdown", MESH_BOOL, wifi_shutdown),
	MFIELD("auto_update", MESH_BOOL, auto_update),
	MFIELD("pkg_url", MESH_STR, pkg_url),
	MFIELD("controller_id", MESH_STR, controller_id),
	MFIELD("controller_name", MESH_STR, controller_name),
	MFIELD("controller_addr", MESH_STR, controller_addr),
	MFIELD("member_id", MESH_STR, member_id)
};

#undef MFIELD

const char *mesh_role_name(enum mesh_role role)
{
	return role == MESH_NODE ? "node" : "controller";
}

const char *mesh_self_node_id(void)
{
	if (mesh.role == MESH_NODE && mesh.member_id[0])
		return mesh.member_id;

	return "controller";
}

static const char *bss_nr_hex(struct blob_attr *nr)
{
	struct blob_attr *cur;
	const char *best = NULL;
	size_t best_len = 0;
	int rem;

	if (!nr)
		return NULL;

	blobmsg_for_each_attr(cur, nr, rem) {
		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING) {
			const char *s = blobmsg_get_string(cur);
			size_t len = strlen(s);

			if (len > best_len) {
				best_len = len;
				best = s;
			}
		}
	}

	return best;
}

void mesh_aps_dump(struct blob_buf *b, const char *name)
{
	struct roam_bss *bss;
	void *arr = blobmsg_open_array(b, name);

	list_for_each_entry(bss, &roam_bss_list, list) {
		const char *nr = bss_nr_hex(bss->nr);
		char bssid[18];
		void *e = blobmsg_open_table(b, NULL);

		snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
			 bss->bssid[0], bss->bssid[1], bss->bssid[2],
			 bss->bssid[3], bss->bssid[4], bss->bssid[5]);

		blobmsg_add_string(b, "ifname", bss->ifname);
		blobmsg_add_string(b, "bssid", bssid);
		blobmsg_add_string(b, "ssid", bss->ssid);
		blobmsg_add_u32(b, "channel", bss->channel);
		blobmsg_add_string(b, "band", roam_band_name(bss->band));
		blobmsg_add_u8(b, "neighbor_report", !!bss->nr);
		if (nr)
			blobmsg_add_string(b, "nr", nr);
		blobmsg_close_table(b, e);
	}

	blobmsg_close_array(b, arr);
}

struct mesh_neighbor {
	char bssid[18];
	char ssid[MESH_SSID_MAX];
	char nr[160];
};

static struct mesh_neighbor mesh_nbr[MESH_NBR_MAX];
static unsigned int mesh_nbr_n;

void mesh_neighbors_set(struct blob_attr *arr)
{
	enum { N_BSSID, N_SSID, N_NR, __N_MAX };
	static const struct blobmsg_policy np[__N_MAX] = {
		[N_BSSID] = { "bssid", BLOBMSG_TYPE_STRING },
		[N_SSID] = { "ssid", BLOBMSG_TYPE_STRING },
		[N_NR] = { "nr", BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *cur;
	int rem;

	mesh_nbr_n = 0;
	if (!arr)
		return;

	blobmsg_for_each_attr(cur, arr, rem) {
		struct blob_attr *tb[__N_MAX];
		struct mesh_neighbor *n;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE || mesh_nbr_n >= MESH_NBR_MAX)
			continue;

		blobmsg_parse(np, __N_MAX, tb, blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[N_BSSID] || !tb[N_NR])
			continue;

		n = &mesh_nbr[mesh_nbr_n++];
		memset(n, 0, sizeof(*n));
		strncpy(n->bssid, blobmsg_get_string(tb[N_BSSID]), sizeof(n->bssid) - 1);
		if (tb[N_SSID])
			strncpy(n->ssid, blobmsg_get_string(tb[N_SSID]), sizeof(n->ssid) - 1);
		strncpy(n->nr, blobmsg_get_string(tb[N_NR]), sizeof(n->nr) - 1);
	}
}

static bool nbr_is_local(const char *bssid)
{
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list) {
		char bs[18];

		snprintf(bs, sizeof(bs), "%02x:%02x:%02x:%02x:%02x:%02x",
			 bss->bssid[0], bss->bssid[1], bss->bssid[2],
			 bss->bssid[3], bss->bssid[4], bss->bssid[5]);
		if (!strcasecmp(bs, bssid))
			return true;
	}

	return false;
}

void mesh_neighbors_append(struct blob_buf *b, const char *ssid, int *count, int max)
{
	unsigned int i;

	for (i = 0; i < mesh_nbr_n && *count < max; i++) {
		struct mesh_neighbor *n = &mesh_nbr[i];
		void *e;

		if (ssid && n->ssid[0] && strcmp(n->ssid, ssid))
			continue;
		if (nbr_is_local(n->bssid))
			continue;

		e = blobmsg_open_array(b, NULL);
		blobmsg_add_string(b, NULL, n->bssid);
		blobmsg_add_string(b, NULL, n->ssid);
		blobmsg_add_string(b, NULL, n->nr);
		blobmsg_close_array(b, e);
		(*count)++;
	}
}

struct assoc_ctx {
	struct mesh_assoc_idx *idx;
	uint8_t band;
	const char *enc;
};

static void assoc_bytes(struct blob_attr *t, uint64_t *out)
{
	static const struct blobmsg_policy bp = { "bytes", BLOBMSG_TYPE_INT64 };
	struct blob_attr *v = NULL;

	if (!t)
		return;

	blobmsg_parse(&bp, 1, &v, blobmsg_data(t), blobmsg_data_len(t));
	if (v)
		*out = blobmsg_get_u64(v);
}

static void assoc_phy(struct blob_attr *rate, struct mesh_assoc *a)
{
	enum { R_RATE, R_MHZ, R_NSS, R_HT, R_VHT, R_HE, __R_MAX };
	static const struct blobmsg_policy rp[__R_MAX] = {
		[R_RATE] = { "rate", BLOBMSG_TYPE_INT32 },
		[R_MHZ] = { "mhz", BLOBMSG_TYPE_INT32 },
		[R_NSS] = { "nss", BLOBMSG_TYPE_INT32 },
		[R_HT] = { "ht", BLOBMSG_TYPE_INT8 },
		[R_VHT] = { "vht", BLOBMSG_TYPE_INT8 },
		[R_HE] = { "he", BLOBMSG_TYPE_INT8 },
	};
	struct blob_attr *tb[__R_MAX];
	const char *std;

	blobmsg_parse(rp, __R_MAX, tb, blobmsg_data(rate), blobmsg_data_len(rate));

	if (tb[R_RATE])
		a->rate = blobmsg_get_u32(tb[R_RATE]);
	if (tb[R_MHZ])
		a->width = blobmsg_get_u32(tb[R_MHZ]);
	if (tb[R_NSS])
		a->nss = blobmsg_get_u32(tb[R_NSS]);

	if (tb[R_HE] && blobmsg_get_u8(tb[R_HE]))
		std = "11ax";
	else if (tb[R_VHT] && blobmsg_get_u8(tb[R_VHT]))
		std = "11ac";
	else if (tb[R_HT] && blobmsg_get_u8(tb[R_HT]))
		std = "11n";
	else
		std = a->band == BAND_LOW ? "11g" : "11a";

	strncpy(a->std, std, sizeof(a->std) - 1);
}

static void assoc_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const struct blobmsg_policy lp = { "results", BLOBMSG_TYPE_ARRAY };
	enum { A_MAC, A_RX, A_TX, __A_MAX };
	static const struct blobmsg_policy ap[__A_MAX] = {
		[A_MAC] = { "mac", BLOBMSG_TYPE_STRING },
		[A_RX] = { "rx", BLOBMSG_TYPE_TABLE },
		[A_TX] = { "tx", BLOBMSG_TYPE_TABLE },
	};
	struct assoc_ctx *ctx = req->priv;
	struct blob_attr *results = NULL, *cur;
	int rem;

	blobmsg_parse(&lp, 1, &results, blob_data(msg), blob_len(msg));
	if (!results)
		return;

	blobmsg_for_each_attr(cur, results, rem) {
		struct blob_attr *tb[__A_MAX];
		struct ether_addr *ea;
		struct mesh_assoc *a;

		if (ctx->idx->n >= MESH_ASSOC_MAX)
			break;

		blobmsg_parse(ap, __A_MAX, tb, blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[A_MAC])
			continue;

		ea = ether_aton(blobmsg_get_string(tb[A_MAC]));
		if (!ea)
			continue;

		a = &ctx->idx->e[ctx->idx->n++];
		memset(a, 0, sizeof(*a));
		memcpy(a->addr, ea->ether_addr_octet, 6);
		a->band = ctx->band;
		if (ctx->enc)
			strncpy(a->enc, ctx->enc, sizeof(a->enc) - 1);
		if (tb[A_TX])
			assoc_phy(tb[A_TX], a);
		assoc_bytes(tb[A_RX], &a->rx_bytes);
		assoc_bytes(tb[A_TX], &a->tx_bytes);
	}
}

static void info_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const struct blobmsg_policy ep = { "encryption", BLOBMSG_TYPE_TABLE };
	static const struct blobmsg_policy wp[2] = {
		{ "wpa", BLOBMSG_TYPE_ARRAY },
		{ "enabled", BLOBMSG_TYPE_INT8 },
	};
	struct blob_attr *enc = NULL, *tb[2], *cur;
	char *out = req->priv;
	int rem, max = 0;

	blobmsg_parse(&ep, 1, &enc, blob_data(msg), blob_len(msg));
	if (!enc)
		return;

	blobmsg_parse(wp, 2, tb, blobmsg_data(enc), blobmsg_data_len(enc));
	if (tb[1] && !blobmsg_get_u8(tb[1]))
		return;

	if (tb[0])
		blobmsg_for_each_attr(cur, tb[0], rem) {
			int v = 0;

			switch (blobmsg_type(cur)) {
			case BLOBMSG_TYPE_INT32: v = blobmsg_get_u32(cur); break;
			case BLOBMSG_TYPE_INT16: v = blobmsg_get_u16(cur); break;
			case BLOBMSG_TYPE_INT8: v = blobmsg_get_u8(cur); break;
			}

			if (v > max)
				max = v;
		}

	strcpy(out, max >= 3 ? "WPA3" : max == 2 ? "WPA2" : max == 1 ? "WPA" : "");
}

bool mesh_parent_mac(uint8_t *out)
{
	char line[256];
	FILE *f;
	bool found = false;

	if (mesh.role != MESH_NODE || !mesh.controller_addr[0])
		return false;

	f = fopen("/proc/net/arp", "r");
	if (!f)
		return false;

	while (!found && fgets(line, sizeof(line), f)) {
		char ip[64], type[16], flags[16], hw[32];
		struct ether_addr ea;

		if (sscanf(line, "%63s %15s %15s %31s", ip, type, flags, hw) != 4)
			continue;

		if (strcmp(ip, mesh.controller_addr) || !strcmp(flags, "0x0"))
			continue;

		if (!ether_aton_r(hw, &ea))
			continue;

		memcpy(out, ea.ether_addr_octet, 6);
		found = true;
	}

	fclose(f);

	return found;
}

void mesh_assoc_collect(struct mesh_assoc_idx *idx)
{
	static struct blob_buf ab;
	struct roam_bss *bss;
	uint32_t iwinfo_id;

	idx->n = 0;
	if (ubus_lookup_id(ubus_ctx, "iwinfo", &iwinfo_id))
		return;

	list_for_each_entry(bss, &roam_bss_list, list) {
		struct assoc_ctx ctx = { .idx = idx, .band = bss->band, .enc = NULL };
		char enc[16] = "";

		if (!bss->active)
			continue;

		if (mesh.backhaul_ssid[0] && !strcmp(bss->ssid, mesh.backhaul_ssid))
			continue;

		blob_buf_init(&ab, 0);
		blobmsg_add_string(&ab, "device", bss->ifname);

		ubus_invoke(ubus_ctx, iwinfo_id, "info", ab.head, info_cb, enc, MESH_UBUS_TIMEOUT);
		ctx.enc = enc[0] ? enc : NULL;
		ubus_invoke(ubus_ctx, iwinfo_id, "assoclist", ab.head, assoc_cb, &ctx, MESH_UBUS_TIMEOUT);
	}
}

void mesh_assoc_blob(struct blob_buf *b, const struct mesh_assoc *a)
{
	if (a->wired) {
		blobmsg_add_u8(b, "wired", 1);
		return;
	}

	if (a->rate)
		blobmsg_add_u32(b, "rate", a->rate);
	if (a->std[0])
		blobmsg_add_string(b, "std", a->std);
	if (a->width)
		blobmsg_add_u32(b, "width", a->width);
	if (a->nss)
		blobmsg_add_u32(b, "nss", a->nss);
	if (a->enc[0])
		blobmsg_add_string(b, "encryption", a->enc);
	if (a->rx_bytes)
		blobmsg_add_u64(b, "rx_bytes", a->rx_bytes);
	if (a->tx_bytes)
		blobmsg_add_u64(b, "tx_bytes", a->tx_bytes);
}

void mesh_members_clear(void)
{
	struct mesh_member *m, *tmp;

	list_for_each_entry_safe(m, tmp, &mesh_members, list) {
		list_del(&m->list);
		free(m);
	}
}

struct mesh_member *mesh_member_by_name(const char *name)
{
	struct mesh_member *m;

	list_for_each_entry(m, &mesh_members, list)
		if (!strcmp(m->name, name))
			return m;

	return NULL;
}

void mesh_config_init(void)
{
	memset(&mesh, 0, sizeof(mesh));

	mesh.enabled = true;
	mesh.role = MESH_CONTROLLER;
	mesh.backhaul_enabled = true;

	mesh_members_clear();
}

static void member_str(struct uci_context *ctx, struct uci_section *s,
		       const char *name, char *dst, size_t size)
{
	const char *v = uci_lookup_option_string(ctx, s, name);

	if (v) {
		strncpy(dst, v, size - 1);
		dst[size - 1] = '\0';
	}
}

void mesh_member_load(struct uci_context *ctx, struct uci_section *s)
{
	const char *managed = uci_lookup_option_string(ctx, s, "managed");
	struct mesh_member *m = calloc(1, sizeof(*m));

	if (!m)
		return;

	member_str(ctx, s, "id", m->id, sizeof(m->id));
	member_str(ctx, s, "name", m->name, sizeof(m->name));
	member_str(ctx, s, "hostname", m->hostname, sizeof(m->hostname));
	member_str(ctx, s, "mac", m->mac, sizeof(m->mac));
	member_str(ctx, s, "addr", m->addr, sizeof(m->addr));
	m->managed = managed ? roam_uci_bool(managed) : true;

	list_add_tail(&m->list, &mesh_members);
}

static void field_apply(const struct mesh_field *f, const char *value)
{
	void *p = (char *)&mesh + f->offset;

	switch (f->type) {
	case MESH_BOOL:
		*(bool *)p = roam_uci_bool(value);
		break;
	case MESH_ROLE:
		*(enum mesh_role *)p = strcmp(value, "node") ? MESH_CONTROLLER : MESH_NODE;
		break;
	case MESH_STR:
		strncpy(p, value, f->size - 1);
		((char *)p)[f->size - 1] = '\0';
		break;
	}
}

void mesh_config_apply(struct uci_context *ctx, struct uci_section *s)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		const char *v = uci_lookup_option_string(ctx, s, fields[i].name);

		if (v)
			field_apply(&fields[i], v);
	}
}

void mesh_uci_set(struct uci_context *ctx, const char *pkg, const char *sect,
		  const char *opt, const char *val)
{
	struct uci_ptr ptr = {
		.package = pkg, .section = sect, .option = opt, .value = val,
	};

	uci_set(ctx, &ptr);
}

void mesh_start(void)
{
	static bool log_loaded;

	if (!mesh.enabled) {
		roam_log(ROAM_L_INFO, "mesh: disabled");
		return;
	}

	if (!log_loaded) {
		mesh_log_load();
		mesh_clients_load();
		log_loaded = true;
	}

	mesh_deps_start();

	if (mesh.role == MESH_NODE) {
		roam_log(ROAM_L_INFO, "mesh: node of controller %s (%s)",
			 mesh.controller_id[0] ? mesh.controller_id : "?",
			 mesh.controller_addr[0] ? mesh.controller_addr : "?");
		mesh_node_watch_start();
	} else {
		mesh_ctrl_ensure_id();
		roam_log(ROAM_L_INFO, "mesh: controller %s", mesh.controller_id);
		mesh_ctrl_backhaul_apply();
		mesh_ctrl_bridge_stp();
		mesh_ctrl_poll_start();
		mesh_ctrl_sync();
	}
}
