#ifndef ROAMD_H
#define ROAMD_H

#include <stdbool.h>
#include <stdint.h>
#include <net/if.h>
#include <syslog.h>

#include <stddef.h>

#include <libubox/avl.h>
#include <libubox/utils.h>
#include <libubox/blobmsg.h>
#include <libubox/list.h>
#include <libubox/uloop.h>
#include <libubus.h>

#ifndef ROAMD_VERSION
#define ROAMD_VERSION		"1.0.0"
#endif

#define ROAMD_SSID_MAX		33
#define ROAMD_MDID_LEN		4
#define ROAMD_NO_SIGNAL		0
#define ROAMD_MAX_NEIGHBORS	8

#define PAIR_NOT_MANAGED	0x0001
#define PAIR_NO_PEER		0x0002
#define PAIR_DISABLED		0x0004
#define PAIR_ENCRYPTION		0x0008
#define PAIR_KEY		0x0010
#define PAIR_NETWORK		0x0020
#define PAIR_NO_11K		0x0040
#define PAIR_NO_11V		0x0080
#define PAIR_FT			0x0100
#define PAIR_NO_NEIGHBOR	0x0200

#define PAIR_FATAL		0x003f

enum roam_band {
	BAND_LOW,
	BAND_HIGH,
	BAND_MAX
};

enum roam_prefer {
	PREFER_NONE,
	PREFER_LOW,
	PREFER_HIGH
};

enum roam_lock {
	LOCK_NONE,
	LOCK_LOW,
	LOCK_HIGH
};

enum roam_event {
	EVENT_PROBE,
	EVENT_AUTH,
	EVENT_ASSOC,
	EVENT_MAX
};

struct roam_config {
	bool enabled;
	bool band_steering;
	bool fast_transition;
	bool ft_over_ds;
	bool neighbor_reports;
	bool bss_transition;
	bool apply_wireless;
	bool allow_kick;
	bool deny_probe;
	enum roam_prefer prefer;
	char ssid[ROAMD_SSID_MAX];
	char mobility_domain[ROAMD_MDID_LEN + 1];

	int rssi_low;
	int rssi_good;
	int rssi_diff;
	int kick_rssi;
	int cross_band_delta;

	uint32_t hold_time;
	uint32_t age_time;
	uint32_t check_time[BAND_MAX];
	uint32_t poll_interval;
	uint32_t deny_time;
	uint32_t kick_delay;
	uint32_t steer_retries;
	uint32_t beacon_req_interval;
	int log_level;
};

struct roam_bss {
	struct avl_node avl;
	struct ubus_subscriber sub;
	struct uloop_timeout poll;
	struct list_head list;

	uint32_t obj_id;
	uint32_t nr_sent;
	uint64_t nr_sent_at;
	char ifname[IFNAMSIZ];
	char ssid[ROAMD_SSID_MAX];
	uint8_t bssid[6];
	int freq;
	int channel;
	int op_class;
	enum roam_band band;
	bool active;
	bool subscribed;
	struct blob_attr *nr;

	uint32_t pair_issues;
	struct roam_bss *pair_peer;
};

struct roam_sta_band {
	int signal;
	uint64_t seen;
	uint64_t deny_start;
	uint64_t allow_until;
	bool ht;
	bool present;
};

struct roam_sta {
	struct avl_node avl;
	uint8_t addr[6];
	char mac[18];

	struct roam_sta_band band[BAND_MAX];
	struct roam_bss *bss;

	uint64_t connected_since;
	uint64_t last_steer;
	uint64_t last_beacon_req;
	uint64_t kick_at;

	uint32_t steer_count;
	uint32_t flap_count;
	uint64_t give_up_until;
	uint8_t steer_from;
	uint32_t beacon_req_silent;
	uint8_t dialog_token;
	bool btm;
	bool rrm;
	bool btm_rejected;
};

extern struct roam_config config;
extern struct ubus_context *ubus_ctx;
extern struct avl_tree roam_bss_tree;
extern struct avl_tree roam_sta_tree;
extern struct list_head roam_bss_list;
extern uint64_t roam_now;

void roam_time_update(void);
const char *roam_band_name(enum roam_band band);

void roam_device_setup(void);
enum roam_lock roam_device_lock(const uint8_t *addr);
bool roam_device_node_allowed(const uint8_t *addr, const char *node_id);
void roam_config_load(void);
void roam_config_dump(struct blob_buf *b);

bool roam_uci_bool(const char *value);
void roam_wireless_apply(void);

extern const char *const roam_pair_issues[];
void roam_pair_evaluate(void);

void roam_bss_setup(void);
void roam_bss_init(void);
void roam_bss_free_all(void);
struct roam_bss *roam_bss_by_bssid(const uint8_t *bssid);
struct roam_bss *roam_bss_target(const struct roam_bss *from);
bool roam_bss_matches(const struct roam_bss *bss);
void roam_bss_recheck(void);
void roam_neighbors_resync(void);
int roam_bss_invoke(struct roam_bss *bss, const char *method, struct blob_buf *buf);

void roam_sta_setup(void);
struct roam_sta *roam_sta_get(const uint8_t *addr, bool create);
void roam_sta_expire(void);
void roam_sta_drop_bss(struct roam_bss *bss);
void roam_sta_event(struct roam_bss *bss, const uint8_t *addr, int signal);
void roam_sta_set_connected(struct roam_sta *sta, struct roam_bss *bss, int signal);
void roam_sta_disconnected(struct roam_sta *sta);
void roam_sta_reset(struct roam_sta *sta);

bool roam_policy_allow(struct roam_sta *sta, struct roam_bss *bss, enum roam_event ev);
void roam_policy_kick(struct roam_sta *sta, struct roam_bss *from);
void roam_policy_run(struct roam_bss *bss);
void roam_policy_btm_response(struct roam_sta *sta, int status);

void roam_ubus_object_init(void);

#define roam_log(level, fmt, ...) do { \
	if (config.log_level >= (level)) \
		syslog(LOG_INFO, fmt, ##__VA_ARGS__); \
} while (0)

#define ROAMD_RCPI_IMPLAUSIBLE	200

#define ROAM_L_ERR	0
#define ROAM_L_INFO	1
#define ROAM_L_DEBUG	2

#endif
