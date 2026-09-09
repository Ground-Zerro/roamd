#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uci.h>

#include "roamd.h"
#include "mesh.h"

struct wireless_setting {
	const char *name;
	const char *value;
};

static bool encryption_supports_ft(const char *enc)
{
	static const char *const prefixes[] = { "psk2", "psk-mixed", "sae", "wpa2", "wpa3", "wpa-mixed" };
	size_t i;

	if (!enc)
		return false;

	for (i = 0; i < ARRAY_SIZE(prefixes); i++) {
		if (!strncmp(enc, prefixes[i], strlen(prefixes[i])))
			return true;
	}

	return false;
}

static bool encryption_is_eap(const char *enc)
{
	return enc && !strncmp(enc, "wpa", 3);
}

static bool option_set_list(struct uci_context *ctx, struct uci_section *s,
			    const char *name, const char *value)
{
	struct uci_ptr cur = {
		.package = s->package->e.name, .section = s->e.name, .option = name,
		.p = s->package, .s = s,
	};
	struct uci_ptr add = cur;
	struct uci_option *o = uci_lookup_option(ctx, s, name);

	if (o && o->type == UCI_TYPE_LIST) {
		struct uci_element *e = list_to_element(o->v.list.next);

		if (o->v.list.next == o->v.list.prev && !strcmp(e->name, value))
			return false;
	}

	uci_delete(ctx, &cur);
	add.value = value;
	uci_add_list(ctx, &add);

	return true;
}

static void mdid_derive(const char *ssid, char *out)
{
	uint32_t hash = 2166136261u;

	while (*ssid) {
		hash ^= (uint8_t)*ssid++;
		hash *= 16777619u;
	}

	hash = (hash ^ (hash >> 16)) & 0xffff;
	if (hash == 0 || hash == 0xffff)
		hash = 0x4b52;

	snprintf(out, ROAMD_MDID_LEN + 1, "%04x", hash);
}

static bool option_set(struct uci_context *ctx, struct uci_section *s,
		       const char *name, const char *value)
{
	struct uci_ptr ptr = {
		.package = s->package->e.name,
		.section = s->e.name,
		.option = name,
		.value = value
	};
	const char *cur = uci_lookup_option_string(ctx, s, name);

	if (cur && !strcmp(cur, value))
		return false;

	ptr.p = s->package;
	ptr.s = s;

	return uci_set(ctx, &ptr) == UCI_OK;
}

static bool iface_provision(struct uci_context *ctx, struct uci_section *s)
{
	const char *mode = uci_lookup_option_string(ctx, s, "mode");
	const char *ssid = uci_lookup_option_string(ctx, s, "ssid");
	const char *disabled = uci_lookup_option_string(ctx, s, "disabled");
	const char *enc = uci_lookup_option_string(ctx, s, "encryption");
	char mdid[ROAMD_MDID_LEN + 1];
	bool ft, changed = false;
	size_t i;

	if (!mode || strcmp(mode, "ap") || !ssid)
		return false;

	if (roam_uci_bool(disabled))
		return false;

	if (config.ssid[0] && strcmp(config.ssid, ssid))
		return false;

	bool eap;

	ft = config.fast_transition && encryption_supports_ft(enc);
	eap = ft && encryption_is_eap(enc) && mesh.ft_key[0];

	if (config.mobility_domain[0])
		snprintf(mdid, sizeof(mdid), "%s", config.mobility_domain);
	else
		mdid_derive(ssid, mdid);

	const struct wireless_setting settings[] = {
		{ "ieee80211k", config.neighbor_reports ? "1" : "0" },
		{ "rrm_neighbor_report", config.neighbor_reports ? "1" : "0" },
		{ "rrm_beacon_report", config.neighbor_reports ? "1" : "0" },
		{ "bss_transition", config.bss_transition ? "1" : "0" },
		{ "wnm_sleep_mode", config.bss_transition ? "1" : "0" },
		{ "ieee80211r", ft ? "1" : "0" },
		{ "ft_over_ds", ft && config.ft_over_ds ? "1" : "0" },
		{ "ft_psk_generate_local", (ft && !eap) ? "1" : "0" },
		{ "mobility_domain", mdid }
	};

	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		if (!ft && !strcmp(settings[i].name, "mobility_domain"))
			continue;

		if (option_set(ctx, s, settings[i].name, settings[i].value))
			changed = true;
	}

	if (eap) {
		char r0kh[80], r1kh[96];

		snprintf(r0kh, sizeof(r0kh), "ff:ff:ff:ff:ff:ff,*,%s", mesh.ft_key);
		snprintf(r1kh, sizeof(r1kh), "00:00:00:00:00:00,00:00:00:00:00:00,%s", mesh.ft_key);

		if (option_set(ctx, s, "nas_identifier", mesh_self_node_id()))
			changed = true;
		if (option_set(ctx, s, "pmk_r1_push", "0"))
			changed = true;
		if (option_set_list(ctx, s, "r0kh", r0kh))
			changed = true;
		if (option_set_list(ctx, s, "r1kh", r1kh))
			changed = true;
	}

	return changed;
}

static void network_reload(void)
{
	uint32_t id;

	if (ubus_lookup_id(ubus_ctx, "network", &id))
		return;

	ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
}

void roam_wireless_apply(void)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	bool changed = false;

	if (!config.apply_wireless)
		return;

	ctx = uci_alloc_context();
	if (!ctx)
		return;

	if (uci_load(ctx, "wireless", &pkg) != UCI_OK) {
		uci_free_context(ctx);
		return;
	}

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (strcmp(s->type, "wifi-iface"))
			continue;

		if (iface_provision(ctx, s))
			changed = true;
	}

	if (changed) {
		uci_commit(ctx, &pkg, false);
		roam_log(ROAM_L_INFO, "roamd: wireless configuration updated, reloading network");
	}

	uci_free_context(ctx);

	if (changed)
		network_reload();
}
