#include <stdlib.h>
#include <string.h>
#include <netinet/ether.h>
#include <uci.h>

#include "roamd.h"
#include "mesh.h"

struct roam_config config;

struct roam_device {
	struct avl_node avl;
	uint8_t addr[6];
	enum roam_lock lock;
	unsigned int n_nodes;
	char nodes[MESH_ALLOW_MAX][MESH_ID_MAX];
};

static struct avl_tree device_tree;

enum opt_type {
	OPT_BOOL,
	OPT_INT,
	OPT_U32,
	OPT_STR,
	OPT_PREFER
};

struct opt_desc {
	const char *name;
	enum opt_type type;
	size_t offset;
	size_t size;
};

#define OPT(n, t, f) { n, t, offsetof(struct roam_config, f), sizeof(((struct roam_config *)0)->f) }

static const struct opt_desc opts[] = {
	OPT("enabled", OPT_BOOL, enabled),
	OPT("band_steering", OPT_BOOL, band_steering),
	OPT("fast_transition", OPT_BOOL, fast_transition),
	OPT("ft_over_ds", OPT_BOOL, ft_over_ds),
	OPT("neighbor_reports", OPT_BOOL, neighbor_reports),
	OPT("bss_transition", OPT_BOOL, bss_transition),
	OPT("apply_wireless", OPT_BOOL, apply_wireless),
	OPT("allow_kick", OPT_BOOL, allow_kick),
	OPT("deny_probe", OPT_BOOL, deny_probe),
	OPT("prefer_band", OPT_PREFER, prefer),
	OPT("ssid", OPT_STR, ssid),
	OPT("mobility_domain", OPT_STR, mobility_domain),
	OPT("rssi_low", OPT_INT, rssi_low),
	OPT("rssi_good", OPT_INT, rssi_good),
	OPT("rssi_diff", OPT_INT, rssi_diff),
	OPT("kick_rssi", OPT_INT, kick_rssi),
	OPT("cross_band_delta", OPT_INT, cross_band_delta),
	OPT("hold_time", OPT_U32, hold_time),
	OPT("age_time", OPT_U32, age_time),
	OPT("check_time_low", OPT_U32, check_time[BAND_LOW]),
	OPT("check_time_high", OPT_U32, check_time[BAND_HIGH]),
	OPT("poll_interval", OPT_U32, poll_interval),
	OPT("deny_time", OPT_U32, deny_time),
	OPT("steer_retries", OPT_U32, steer_retries),
	OPT("beacon_req_interval", OPT_U32, beacon_req_interval),
	OPT("log_level", OPT_INT, log_level)
};

#undef OPT

static void roam_config_init(void)
{
	memset(&config, 0, sizeof(config));

	config.enabled = true;
	config.band_steering = true;
	config.fast_transition = true;
	config.ft_over_ds = true;
	config.neighbor_reports = true;
	config.bss_transition = true;
	config.apply_wireless = true;
	config.allow_kick = false;
	config.deny_probe = true;
	config.prefer = PREFER_HIGH;

	config.rssi_low = -75;
	config.rssi_good = -55;
	config.rssi_diff = 30;
	config.kick_rssi = -82;
	config.cross_band_delta = 8;

	config.hold_time = 30000;
	config.age_time = 90000;
	config.check_time[BAND_LOW] = 8000;
	config.check_time[BAND_HIGH] = 4000;
	config.poll_interval = 2000;
	config.deny_time = 15000;
	config.steer_retries = 3;
	config.beacon_req_interval = 20000;
	config.log_level = ROAM_L_INFO;
}

bool roam_uci_bool(const char *value)
{
	if (!value)
		return false;

	return !strcmp(value, "1") || !strcmp(value, "true") ||
	       !strcmp(value, "yes") || !strcmp(value, "on");
}

static enum roam_prefer opt_to_prefer(const char *v)
{
	if (!strcmp(v, "5") || !strcmp(v, "high"))
		return PREFER_HIGH;
	if (!strcmp(v, "2") || !strcmp(v, "2.4") || !strcmp(v, "low"))
		return PREFER_LOW;
	return PREFER_NONE;
}

static void opt_apply(const struct opt_desc *o, const char *value)
{
	void *field = (char *)&config + o->offset;

	switch (o->type) {
	case OPT_BOOL:
		*(bool *)field = roam_uci_bool(value);
		break;
	case OPT_INT:
		*(int *)field = (int)strtol(value, NULL, 10);
		break;
	case OPT_U32:
		*(uint32_t *)field = (uint32_t)strtoul(value, NULL, 10);
		break;
	case OPT_STR:
		snprintf(field, o->size, "%s", value);
		break;
	case OPT_PREFER:
		*(enum roam_prefer *)field = opt_to_prefer(value);
		break;
	}
}

static int device_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, 6);
}

void roam_device_setup(void)
{
	avl_init(&device_tree, device_cmp, false, NULL);
}

static void devices_clear(void)
{
	struct roam_device *dev, *tmp;

	avl_for_each_element_safe(&device_tree, dev, avl, tmp) {
		avl_delete(&device_tree, &dev->avl);
		free(dev);
	}
}

enum roam_lock roam_device_lock(const uint8_t *addr)
{
	struct roam_device *dev = avl_find_element(&device_tree, addr, dev, avl);

	return dev ? dev->lock : LOCK_NONE;
}

bool roam_device_node_allowed(const uint8_t *addr, const char *node_id)
{
	struct roam_device *dev = avl_find_element(&device_tree, addr, dev, avl);
	unsigned int i;

	if (!dev || !dev->n_nodes)
		return true;

	for (i = 0; i < dev->n_nodes; i++)
		if (!strcmp(dev->nodes[i], node_id))
			return true;

	return false;
}

static enum roam_lock opt_to_lock(const char *v)
{
	if (!strcmp(v, "5") || !strcmp(v, "high"))
		return LOCK_HIGH;
	if (!strcmp(v, "2") || !strcmp(v, "2.4") || !strcmp(v, "low"))
		return LOCK_LOW;

	return LOCK_NONE;
}

static void device_add_node(struct roam_device *dev, const char *node)
{
	if (!node || !node[0] || dev->n_nodes >= MESH_ALLOW_MAX)
		return;

	strncpy(dev->nodes[dev->n_nodes], node, MESH_ID_MAX - 1);
	dev->nodes[dev->n_nodes][MESH_ID_MAX - 1] = '\0';
	dev->n_nodes++;
}

static void device_load(struct uci_context *ctx, struct uci_section *s)
{
	const char *mac = uci_lookup_option_string(ctx, s, "mac");
	const char *band = uci_lookup_option_string(ctx, s, "band");
	struct uci_option *node = uci_lookup_option(ctx, s, "node");
	struct roam_device *dev;
	struct ether_addr *ea;
	enum roam_lock lock = band ? opt_to_lock(band) : LOCK_NONE;

	if (!mac)
		return;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return;

	dev->lock = lock;
	if (node && node->type == UCI_TYPE_LIST) {
		struct uci_element *e;

		uci_foreach_element(&node->v.list, e)
			device_add_node(dev, e->name);
	} else if (node && node->type == UCI_TYPE_STRING) {
		device_add_node(dev, node->v.string);
	}

	if (lock == LOCK_NONE && !dev->n_nodes) {
		free(dev);
		return;
	}

	ea = ether_aton(mac);
	if (!ea) {
		free(dev);
		return;
	}

	memcpy(dev->addr, ea->ether_addr_octet, 6);
	dev->avl.key = dev->addr;

	if (avl_insert(&device_tree, &dev->avl))
		free(dev);
}

void roam_config_dump(struct blob_buf *b)
{
	char value[64];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(opts); i++) {
		const struct opt_desc *o = &opts[i];
		const void *field = (const char *)&config + o->offset;

		switch (o->type) {
		case OPT_BOOL:
			snprintf(value, sizeof(value), "%u", *(const bool *)field);
			break;
		case OPT_INT:
			snprintf(value, sizeof(value), "%d", *(const int *)field);
			break;
		case OPT_U32:
			snprintf(value, sizeof(value), "%u", *(const uint32_t *)field);
			break;
		case OPT_STR:
			snprintf(value, sizeof(value), "%s", (const char *)field);
			break;
		case OPT_PREFER:
			snprintf(value, sizeof(value), "%s",
				 *(const enum roam_prefer *)field == PREFER_HIGH ? "5" :
				 *(const enum roam_prefer *)field == PREFER_LOW ? "2.4" : "none");
			break;
		}

		blobmsg_add_string(b, o->name, value);
	}
}

static void section_load(struct uci_context *ctx, struct uci_section *s)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(opts); i++) {
		const char *v = uci_lookup_option_string(ctx, s, opts[i].name);

		if (v)
			opt_apply(&opts[i], v);
	}
}

static void config_sanitize(void)
{
	if (config.rssi_good <= config.rssi_low)
		config.rssi_good = config.rssi_low + 10;
	if (config.rssi_diff < 1)
		config.rssi_diff = 1;
	if (config.poll_interval < 500)
		config.poll_interval = 500;
	if (config.age_time < config.hold_time)
		config.age_time = config.hold_time * 3;
	if (!config.steer_retries)
		config.steer_retries = 1;
}

void roam_config_load(void)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct uci_element *e;

	roam_config_init();
	mesh_config_init();

	ctx = uci_alloc_context();
	if (!ctx)
		return;

	devices_clear();

	if (uci_load(ctx, "roamd", &pkg) == UCI_OK) {
		uci_foreach_element(&pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);

			if (!strcmp(s->type, "device"))
				device_load(ctx, s);
			else if (!strcmp(s->type, "mesh"))
				mesh_config_apply(ctx, s);
			else if (!strcmp(s->type, "member"))
				mesh_member_load(ctx, s);
			else
				section_load(ctx, s);
		}
	}

	uci_free_context(ctx);
	config_sanitize();
}
