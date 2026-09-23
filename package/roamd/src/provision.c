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

static bool encryption_is_psk(const char *enc)
{
	return enc && !strncmp(enc, "psk", 3);
}

static void option_set_lines(struct uci_session *u, struct uci_section *s,
			     const char *name, char values[][MESH_FT_KEY_HEX + 40],
			     unsigned int count)
{
	struct uci_option *o = uci_lookup_option(u->ctx, s, name);
	unsigned int i = 0;

	if (o && o->type == UCI_TYPE_LIST) {
		struct uci_element *e;

		uci_foreach_element(&o->v.list, e) {
			if (i >= count || strcmp(e->name, values[i]))
				break;
			i++;
		}

		if (i == count)
			return;
	}

	uci_session_delete(u, s->e.name, name);

	for (i = 0; i < count; i++)
		uci_session_add_list(u, s->e.name, name, values[i]);
}

static void holders_set(struct uci_session *u, struct uci_section *s)
{
	char r0[MESH_FT_KH_MAX][MESH_FT_KEY_HEX + 40];
	char r1[MESH_FT_KH_MAX][MESH_FT_KEY_HEX + 40];
	char list[MESH_FT_KH_LEN];
	unsigned int count = 0;
	char *tok, *save = NULL;

	if (!mesh.ft_kh[0]) {
		snprintf(r0[0], sizeof(r0[0]), "ff:ff:ff:ff:ff:ff,*,%s", mesh.ft_key);
		snprintf(r1[0], sizeof(r1[0]), "00:00:00:00:00:00,00:00:00:00:00:00,%s",
			 mesh.ft_key);

		option_set_lines(u, s, "r0kh", r0, 1);
		option_set_lines(u, s, "r1kh", r1, 1);
		uci_session_set(u, s->e.name, "pmk_r1_push", "0");

		return;
	}

	snprintf(list, sizeof(list), "%s", mesh.ft_kh);

	for (tok = strtok_r(list, " ", &save); tok && count < MESH_FT_KH_MAX;
	     tok = strtok_r(NULL, " ", &save)) {
		char *nasid = strchr(tok, '|');

		if (!nasid)
			continue;

		*nasid++ = '\0';
		snprintf(r0[count], sizeof(r0[0]), "%s,%s,%s", tok, nasid, mesh.ft_key);
		snprintf(r1[count], sizeof(r1[0]), "%s,%s,%s", tok, tok, mesh.ft_key);
		count++;
	}

	option_set_lines(u, s, "r0kh", r0, count);
	option_set_lines(u, s, "r1kh", r1, count);
	uci_session_set(u, s->e.name, "pmk_r1_push", "1");
}

static void mdid_derive(const char *ssid, char *out)
{
	snprintf(out, ROAMD_MDID_LEN + 1, "%04x", roam_hash16(ssid));
}

static void iface_provision(struct uci_session *u, struct uci_section *s)
{
	const char *mode = uci_lookup_option_string(u->ctx, s, "mode");
	const char *ssid = uci_lookup_option_string(u->ctx, s, "ssid");
	const char *disabled = uci_lookup_option_string(u->ctx, s, "disabled");
	const char *enc = uci_lookup_option_string(u->ctx, s, "encryption");
	const struct mesh_network *net;
	char mdid[ROAMD_MDID_LEN + 1];
	bool ft, roaming;
	size_t i;

	if (!mode || strcmp(mode, "ap") || !ssid)
		return;

	if (roam_uci_bool(disabled))
		return;

	net = mesh_network_by_ssid(ssid);
	roaming = !net || net->roaming;

	bool psk, rrb;

	ft = roaming && config.fast_transition && encryption_supports_ft(enc);
	psk = ft && encryption_is_psk(enc);
	rrb = ft && !psk && mesh.ft_key[0];

	if (config.mobility_domain[0])
		snprintf(mdid, sizeof(mdid), "%s", config.mobility_domain);
	else
		mdid_derive(ssid, mdid);

	const struct wireless_setting settings[] = {
		{ "ieee80211k", roaming && config.neighbor_reports ? "1" : "0" },
		{ "rrm_neighbor_report", roaming && config.neighbor_reports ? "1" : "0" },
		{ "rrm_beacon_report", roaming && config.neighbor_reports ? "1" : "0" },
		{ "bss_transition", roaming && config.bss_transition ? "1" : "0" },
		{ "wnm_sleep_mode", roaming && config.bss_transition ? "1" : "0" },
		{ "ieee80211r", ft ? "1" : "0" },
		{ "ft_over_ds", ft && config.ft_over_ds ? "1" : "0" },
		{ "ft_psk_generate_local", psk ? "1" : "0" },
		{ "mobility_domain", mdid }
	};

	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		if (!ft && !strcmp(settings[i].name, "mobility_domain"))
			continue;

		uci_session_set(u, s->e.name, settings[i].name, settings[i].value);
	}

	if (rrb) {
		char nasid[MESH_NASID_MAX];
		uint8_t band = mesh_radio_band(u->ctx, u->pkg,
					       uci_lookup_option_string(u->ctx, s, "device"));

		mesh_ft_nasid(mesh_self_node_id(), band, nasid, sizeof(nasid));
		uci_session_set(u, s->e.name, "nasid", nasid);
		holders_set(u, s);
	}
}

void roam_network_reload(void)
{
	uint32_t id;

	if (ubus_lookup_id(ubus_ctx, "network", &id))
		return;

	ubus_invoke(ubus_ctx, id, "reload", NULL, NULL, NULL, 1000);
}

void roam_wireless_apply(void)
{
	struct uci_session u;
	struct uci_element *e;
	bool changed;

	if (!config.apply_wireless)
		return;

	if (!uci_session_open(&u, "wireless"))
		return;

	uci_foreach_element(&u.pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "wifi-iface"))
			iface_provision(&u, s);
	}

	changed = u.dirty;
	uci_session_close(&u);

	if (!changed)
		return;

	roam_log(ROAM_L_INFO, "roamd: wireless configuration updated, reloading network");
	roam_network_reload();
}
