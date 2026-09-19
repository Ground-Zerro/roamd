#ifndef ROAMD_MESH_H
#define ROAMD_MESH_H

#include <stdbool.h>
#include <uci.h>
#include <libubox/list.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#define MESH_SSID_MAX		33
#define MESH_KEY_MAX		65
#define MESH_ID_MAX		33
#define MESH_BH_PREFIX		"mesh_bh_"
#define MESH_ALLOW_MAX		16
#define MESH_NAME_MAX		64
#define MESH_ADDR_MAX		64
#define MESH_MAC_MAX		18
#define MESH_URL_MAX		192
#define MESH_WORD_MAX		12
#define MESH_ARCH_MAX		24
#define MESH_BH_ALLOW_LEN	(MESH_ALLOW_MAX * (MESH_ID_MAX + 1))
#define MESH_BH_LIMITS_LEN	768
#define MESH_IFACE_MAX		16
#define MESH_PKG_URL_DEFAULT	"https://github.com/Ground-Zerro/roamd/releases/download/feed-%b-%a"

enum mesh_role {
	MESH_CONTROLLER,
	MESH_NODE
};

struct mesh_member {
	struct list_head list;
	char id[MESH_ID_MAX];
	char name[MESH_NAME_MAX];
	char hostname[MESH_NAME_MAX];
	char mac[MESH_MAC_MAX];
	char addr[MESH_ADDR_MAX];
	char bh_band[4];
	char bh_nodes[MESH_BH_ALLOW_LEN];
	bool managed;
};

extern struct list_head mesh_members;

enum mesh_event_type {
	MESH_EV_CONNECT,
	MESH_EV_DISCONNECT,
	MESH_EV_ROAM,
	MESH_EV_STEER,
	MESH_EV_KICK
};

struct mesh_event {
	uint32_t seq;
	uint32_t ts;
	char mac[MESH_MAC_MAX];
	char from_node[MESH_NAME_MAX];
	char to_node[MESH_NAME_MAX];
	uint8_t from_band;
	uint8_t to_band;
	uint8_t type;
	char origin[MESH_ID_MAX];
	uint32_t origin_seq;
};

#define MESH_REPORT_EVENTS	100

#define MESH_BH_DELTA_DEFAULT		20
#define MESH_BH_MIN_SIGNAL_DEFAULT	-78
#define MESH_BH_PARENT_MAX		24
#define MESH_BH_PARENTS_LEN		1024
#define MESH_FT_KEY_HEX			64

#define MESH_NET_MAX		8
#define MESH_NET_ID_LEN		5
#define MESH_NET_SUM_LEN	9
#define MESH_ENC_MAX		24
#define MESH_BAND_24		0x1
#define MESH_BAND_5		0x2

struct mesh_network {
	char id[MESH_NET_ID_LEN];
	char ssid[MESH_SSID_MAX];
	char encryption[MESH_ENC_MAX];
	char key[MESH_KEY_MAX];
	char network[MESH_IFACE_MAX];
	uint8_t bands;
	uint16_t vid;
	bool roaming;
	bool hidden;
	bool isolate;
	bool segment;
};

#define MESH_SEG_VID_MIN	3
#define MESH_SEG_VID_MAX	15
#define MESH_SEG_BRIDGE		"br-seg"
#define MESH_SEG_PREFIX		"sg"

struct mesh_config {
	bool enabled;
	enum mesh_role role;

	bool backhaul_enabled;
	char backhaul_ssid[MESH_SSID_MAX];
	char backhaul_key[MESH_KEY_MAX];
	char backhaul_parents[MESH_BH_PARENTS_LEN];
	char bh_limits[MESH_BH_LIMITS_LEN];
	char ft_key[MESH_FT_KEY_HEX + 1];
	bool wifi_shutdown;
	bool node_ui;
	int backhaul_delta;
	int backhaul_min_signal;
	bool auto_update;
	uint32_t auto_update_every;
	char auto_update_unit[MESH_WORD_MAX];
	uint32_t auto_update_last;
	char auto_update_result[MESH_WORD_MAX];
	char pkg_url[MESH_URL_MAX];

	char controller_id[MESH_ID_MAX];
	char controller_name[MESH_NAME_MAX];
	char controller_addr[MESH_ADDR_MAX];
	char controller_mac[MESH_MAC_MAX];
	char member_id[MESH_ID_MAX];
	char networks_sum[MESH_NET_SUM_LEN];
};

extern struct mesh_config mesh;

void mesh_config_init(void);
void mesh_config_apply(struct uci_context *ctx, struct uci_section *s);
void mesh_member_load(struct uci_context *ctx, struct uci_section *s);
struct mesh_member *mesh_member_by_name(const char *name);
void mesh_members_clear(void);
void mesh_start(void);

const char *mesh_role_name(enum mesh_role role);
const char *mesh_self_node_id(void);
void mesh_aps_dump(struct blob_buf *b, const char *name);

unsigned int mesh_networks_collect(void);
const struct mesh_network *mesh_network_at(unsigned int i);
const struct mesh_network *mesh_network_by_ssid(const char *ssid);
const char *mesh_networks_sum(void);
void mesh_networks_dump(struct blob_buf *b, const char *name);
bool mesh_networks_apply(struct blob_attr *networks);
bool mesh_network_flag(const char *ssid, bool roaming);

bool mesh_link_del(const char *name);
bool mesh_link_exists(const char *name);
bool mesh_link_enslave(const char *parent, int vid, const char *name, const char *bridge);

bool mesh_bridge_wired_port(const char *bridge, char *out, size_t len);
bool mesh_bridge_lan(char *out, size_t len);
unsigned int mesh_bridge_ports(const char *bridge, char out[][IFNAMSIZ], unsigned int max);

typedef void (*mesh_rpc_cb)(void *priv, struct blob_attr *result, bool ok);

bool mesh_rpc_call(const char *id, const char *addr, const char *object,
		   const char *method, const char *args, mesh_rpc_cb cb, void *priv);
void mesh_rpc_forget(const char *id);

#define MESH_PKG_MAIN	"roamd"
#define MESH_PKG_UI	"luci-app-roamd"

struct mesh_pkg_meta {
	char file[MESH_NAME_MAX];
	char sum[65];
	char version[MESH_WORD_MAX * 3];
	bool adb;
};

typedef void (*mesh_pkg_cb)(void *priv, bool ok, char *data, size_t len);
typedef void (*mesh_pkg_ready)(void *priv, bool ok);

bool mesh_pkg_get(const char *url, const char *path, bool pinned, mesh_pkg_cb cb, void *priv);
bool mesh_pkg_feed_url(const char *branch, const char *arch, char *out, size_t len);
bool mesh_pkg_refresh(const char *branch, const char *arch, const char *name,
		      mesh_pkg_ready cb, void *priv);
bool mesh_pkg_known(const char *branch, const char *arch, const char *name,
		    struct mesh_pkg_meta *out);
bool mesh_pkg_stale(const char *branch, const char *arch, const char *name);
int mesh_pkg_newer(const char *have, const char *want);
bool mesh_pkg_path(const char *branch, const char *arch, const char *name, char *out, size_t len);
bool mesh_pkg_download(const char *branch, const char *arch, const char *name,
		       mesh_pkg_ready cb, void *priv);

bool mesh_port_open(const char *addr, uint16_t port, int timeout_ms);
int mesh_run(char *const argv[], int in_fd, char *out, size_t len, int timeout_ms);
int mesh_ssh(const char *addr, const char *cmd, char *out, size_t len, int timeout_ms);
int mesh_ssh_pass(const char *addr, const char *cmd, char *out, size_t len, int timeout_ms);
bool mesh_scp(const char *src, const char *addr, const char *dst);
bool mesh_scp_from(const char *addr, const char *src, const char *dst);
void mesh_node_forget(const char *addr);

#define MESH_NODE_NO_PACKAGE	2
#define MESH_NODE_UNTRUSTED	3
#define MESH_NODE_FROM_CACHE	4
#define MESH_NODE_UNREACHABLE	5
#define MESH_NODE_INSTALL_FAILED 1

bool mesh_release_parse(char *data, char *branch, size_t bl, char *arch, size_t al);
bool mesh_node_release(const char *addr, bool pass, char *branch, size_t bl,
		       char *arch, size_t al);
int mesh_node_install(const char *addr, const char *branch, const char *arch, bool force);
int mesh_node_update(const char *addr, const char *id);
const char *mesh_node_update_step(int rc);
const char *mesh_node_update_error(int rc);
void mesh_member_pkg_source(const char *id, const char *source);
bool mesh_pkg_sync(const char *branch, const char *arch, const char *name);

void mesh_task_report(const char *task, const char *step, const char *state, const char *text);
void mesh_task_open(const char *task);
bool mesh_sys_apk(void);
bool mesh_sys_installed(const char *name, char *out, size_t len);
bool mesh_self_release(char *branch, size_t bl, char *arch, size_t al);
void mesh_self_check(void);
int mesh_self_update(const char *task);
int mesh_node_update_task(const char *addr, const char *task);
int mesh_autoupdate_task(const char *phase, const char *task);
int mesh_update_nodes(const char *task, bool updated);
bool mesh_self_check_busy(void);
void mesh_unquote(char *s);

struct mesh_neigh {
	char addr[MESH_ADDR_MAX];
	char mac[MESH_MAC_MAX];
	bool v6;
	bool ll;
};

unsigned int mesh_neigh_dump(const char *ifname, struct mesh_neigh *out, unsigned int max);
void mesh_neigh_warm(const char *ifname, const char *base);
int mesh_discover_run(void);

bool mesh_lan_cidr(char *out, size_t len);
bool mesh_cidr_netmask(const char *cidr, char *out, size_t len);
bool mesh_lease_reserve(const char *cidr, const char *mac, const char *id,
			char *out, size_t len);
void mesh_lease_drop(const char *id);
void mesh_dnsmasq_reload(void);
bool mesh_ssh_key_ensure(void);
bool mesh_ssh_pubkey(char *out, size_t len);
bool mesh_node_wait_at(const char *addr, const char *mac, int seconds);
void mesh_member_register(const char *id, const char *mac, const char *addr,
			  const char *hostname, const char *label);
int mesh_acquire_run(const char *addr, const char *task);
int mesh_release_run(const char *id, const char *task);
int mesh_deps_ensure(bool need_only);
bool mesh_pkg_probe(const char *url);
bool mesh_random_hex(char *out, size_t bytes);
void mesh_ubus_reload(void);
bool mesh_ubus_call(const char *method, const char *json);

void mesh_poll_run(bool force);
bool mesh_poll_busy(void);

void mesh_seg_links_sync(void);
bool mesh_seg_node_apply(void);
bool mesh_seg_node_drop(void);
const char *mesh_seg_node_issue(void);
const char *mesh_seg_node_trunk(void);
bool mesh_member_state(const char *id, const char *field, char *out, size_t len);
bool mesh_seg_usable(uint16_t vid);
void mesh_seg_node_penalise(uint64_t until);
void mesh_profile_build(struct blob_buf *b);
void mesh_backhaul_bss_set(bool up);

#define MESH_ASSOC_MAX	128

struct mesh_assoc {
	uint8_t addr[6];
	uint32_t rate;
	uint32_t width;
	uint32_t nss;
	uint32_t connected;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	char std[8];
	char enc[16];
	uint8_t band;
	bool wired;
};

struct mesh_assoc_idx {
	struct mesh_assoc e[MESH_ASSOC_MAX];
	unsigned int n;
};

#define MESH_REPORT_MAX	65536

char *mesh_slurp(const char *path, size_t max);

#define MESH_BRPORT_MAX	16

struct mesh_uplink {
	char ifname[16];
	bool wireless;
};

bool mesh_bridge_port_of(const uint8_t *mac, struct mesh_uplink *out);
void mesh_wired_collect(struct mesh_assoc_idx *idx, const uint8_t *parent);
const char *mesh_bridge_member_behind(const uint8_t *mac);
bool mesh_parent_mac(uint8_t *out);
void mesh_bridge_wifi_cost(void);
void mesh_bridge_stp_root(const char *bridge);
bool mesh_bridge_root_is_self(const char *bridge);
bool mesh_bridge_carrier(const char *ifname);
bool mesh_bridge_sta_up(void);
bool mesh_bridge_sta_name(char *out, size_t len);

#define MESH_DEPS_SCRIPT	"/usr/sbin/roamd"
#define MESH_DEPS_STATE		"/tmp/roamd/deps"

struct mesh_task {
	struct uloop_process proc;
	bool busy;
	void (*done)(struct mesh_task *task, int ret);
};

pid_t mesh_spawn(const char *script, const char *arg1, const char *arg2);
#define MESH_RUN_DIR		"/var/run/roamd"

void mesh_dir_ensure(const char *path);

struct ustream_ssl_ctx;
struct ustream_ssl_ops;

struct ustream_ssl_ctx *mesh_ssl_context(const struct ustream_ssl_ops **ops);
pid_t mesh_spawn_argv(char *const argv[]);
bool mesh_task_run(struct mesh_task *task, char *const argv[]);
bool mesh_task_start(struct mesh_task *task, const char *script,
		     const char *arg1, const char *arg2);
void mesh_task_stop(struct mesh_task *task);
void mesh_deps_start(void);
void mesh_deps_blob(struct blob_buf *b);

void mesh_assoc_collect(struct mesh_assoc_idx *idx);
void mesh_assoc_blob(struct blob_buf *b, const struct mesh_assoc *a);

void mesh_clients_dump(struct blob_buf *b);
unsigned int mesh_clients_count(void);
unsigned int mesh_clients_local(void);
bool mesh_client_forget(const char *mac);
void mesh_clients_load(void);
void mesh_clients_save(void);
bool mesh_client_set(const char *mac, const char *band, const char *alias, const char *nodes);

#define MESH_NBR_MAX	64

void mesh_neighbors_set(struct blob_attr *arr);
void mesh_neighbors_append(struct blob_buf *b, const char *ssid, int *count, int max);

enum mesh_job_kind {
	MESH_JOB_DISCOVER,
	MESH_JOB_SELF_CHECK,
};

void mesh_ctrl_job(enum mesh_job_kind kind, bool start, bool stop, struct blob_buf *b);
void mesh_ctrl_acquire(const char *addr, struct blob_buf *b);
void mesh_ctrl_acquire_blob(struct blob_buf *b);
bool mesh_ctrl_acquire_mac(uint8_t *out);
void mesh_ctrl_update(const char *id, struct blob_buf *b);
void mesh_ctrl_self_update(struct blob_buf *b);
void mesh_ctrl_release(const char *id, struct blob_buf *b);
void mesh_ctrl_acquire_status(const char *task, struct blob_buf *b);
void mesh_ctrl_poll_start(void);
void mesh_ctrl_autoupdate_arm(void);
void mesh_ctrl_sync(void);
void mesh_ctrl_ensure_id(void);
void mesh_ctrl_backhaul_apply(void);
void mesh_ctrl_bridge_stp(void);
void mesh_member_live(const char *id, struct blob_buf *b);
bool mesh_member_remove(const char *id);
bool mesh_member_set(const char *id, const char *name, const char *band, const char *nodes);

bool mesh_node_apply(struct blob_attr *msg);
void mesh_node_report(struct blob_buf *b);
void mesh_self_info(struct blob_buf *b);
void mesh_node_diag(struct blob_buf *b);
bool mesh_node_steer(const char *macstr, struct blob_attr *neighbors);
bool mesh_node_drop(const char *macstr);
void mesh_node_touch(void);
uint32_t mesh_node_contact_age(void);
void mesh_node_watch_start(void);
void mesh_node_dumbap(void);
bool mesh_icmp_ping(const char *addr, int timeout_ms);
int mesh_net_apply(const char *bridge, const char *controller);

void mesh_log_local(const char *mac, uint8_t from_band, uint8_t to_band,
		    enum mesh_event_type type);
void mesh_log_push(const struct mesh_event *ev);
void mesh_log_dump(struct blob_buf *b, unsigned int limit);
void mesh_log_ingest(const char *id, const char *node, struct blob_attr *events);
void mesh_log_forget(const char *id);
void mesh_log_load(void);
void mesh_log_flush(void);
const char *mesh_event_type_name(enum mesh_event_type t);

#define MESH_BAND_NA	0xff

#endif
