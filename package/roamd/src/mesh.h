#ifndef ROAMD_MESH_H
#define ROAMD_MESH_H

#include <stdbool.h>
#include <uci.h>
#include <libubox/list.h>
#include <libubox/blobmsg.h>

#define MESH_SSID_MAX		33
#define MESH_KEY_MAX		65
#define MESH_ID_MAX		33
#define MESH_BH_PREFIX		"mesh_bh_"
#define MESH_ALLOW_MAX		16
#define MESH_NAME_MAX		64
#define MESH_ADDR_MAX		64
#define MESH_MAC_MAX		18

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
	bool managed;
	uint32_t log_seq;
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
};

struct mesh_config {
	bool enabled;
	enum mesh_role role;

	bool backhaul_enabled;
	char backhaul_ssid[MESH_SSID_MAX];
	char backhaul_key[MESH_KEY_MAX];
	char backhaul_bssids[128];
	char ft_key[33];
	bool wifi_shutdown;
	bool auto_update;
	char pkg_url[MESH_ADDR_MAX];

	char controller_id[MESH_ID_MAX];
	char controller_name[MESH_NAME_MAX];
	char controller_addr[MESH_ADDR_MAX];
	char member_id[MESH_ID_MAX];
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

#define MESH_ASSOC_MAX	128

struct mesh_assoc {
	uint8_t addr[6];
	uint32_t rate;
	uint32_t width;
	uint32_t nss;
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

#define MESH_DEPS_SCRIPT	"/usr/libexec/roamd/deps-ensure.sh"
#define MESH_DEPS_STATE		"/tmp/roamd/deps"

pid_t mesh_spawn(const char *script, const char *arg1, const char *arg2);
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

void mesh_ctrl_discover(struct blob_buf *b);
void mesh_ctrl_acquire(const char *addr, struct blob_buf *b);
void mesh_ctrl_acquire_blob(struct blob_buf *b);
bool mesh_ctrl_acquire_mac(uint8_t *out);
void mesh_ctrl_update(const char *id, struct blob_buf *b);
void mesh_ctrl_release(const char *id, struct blob_buf *b);
void mesh_ctrl_acquire_status(const char *task, struct blob_buf *b);
void mesh_ctrl_poll_start(void);
void mesh_ctrl_sync(void);
void mesh_uci_set(struct uci_context *ctx, const char *pkg, const char *sect,
		  const char *opt, const char *val);
void mesh_uci_section(struct uci_context *ctx, struct uci_package *pkg);
void mesh_ctrl_ensure_id(void);
void mesh_ctrl_backhaul_apply(void);
void mesh_ctrl_bridge_stp(void);
void mesh_member_live(const char *id, struct blob_buf *b);
bool mesh_member_remove(const char *id);
bool mesh_member_rename(const char *id, const char *name);

bool mesh_node_apply(struct blob_attr *msg);
void mesh_node_report(struct blob_buf *b);
void mesh_self_info(struct blob_buf *b);
void mesh_node_diag(struct blob_buf *b);
bool mesh_node_steer(const char *macstr, struct blob_attr *neighbors);
void mesh_node_touch(void);
uint32_t mesh_node_contact_age(void);
void mesh_node_watch_start(void);

void mesh_log_local(const char *mac, uint8_t from_band, uint8_t to_band,
		    enum mesh_event_type type);
void mesh_log_push(const struct mesh_event *ev);
void mesh_log_dump(struct blob_buf *b, unsigned int limit);
void mesh_log_since(struct blob_buf *b, uint32_t after);
void mesh_log_merge(const char *node, const char *mac, uint32_t ts,
		    const char *from_band, const char *to_band, const char *type);
void mesh_log_ingest(const char *node, struct blob_attr *events);
void mesh_log_load(void);
void mesh_log_flush(void);
const char *mesh_event_type_name(enum mesh_event_type t);

#define MESH_BAND_NA	0xff

#endif
