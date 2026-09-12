#include <stdio.h>
#include <string.h>
#include <uci.h>

#include "roamd.h"

const char *const roam_pair_issues[] = {
	"not_managed",
	"no_peer",
	"disabled",
	"encryption",
	"key",
	"network",
	"no_11k",
	"no_11v",
	"fast_transition",
	"no_neighbor_report",
	NULL
};

struct iface_conf {
	const char *encryption;
	const char *key;
	const char *network;
	const char *mobility_domain;
	bool ft;
};

static bool same_string(const char *a, const char *b)
{
	return !strcmp(a ? a : "", b ? b : "");
}

static void iface_read(struct uci_context *ctx, struct uci_section *s, struct iface_conf *out)
{
	out->encryption = uci_lookup_option_string(ctx, s, "encryption");
	out->key = uci_lookup_option_string(ctx, s, "key");
	out->network = uci_lookup_option_string(ctx, s, "network");
	out->mobility_domain = uci_lookup_option_string(ctx, s, "mobility_domain");
	out->ft = roam_uci_bool(uci_lookup_option_string(ctx, s, "ieee80211r"));
}

static uint32_t ssid_issues(struct uci_context *ctx, struct uci_package *pkg, const char *ssid)
{
	struct iface_conf first = { 0 };
	struct uci_element *e;
	uint32_t issues = 0;
	int count = 0;

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		struct iface_conf cur;
		const char *value;

		if (strcmp(s->type, "wifi-iface"))
			continue;

		value = uci_lookup_option_string(ctx, s, "mode");
		if (!value || strcmp(value, "ap"))
			continue;

		value = uci_lookup_option_string(ctx, s, "ssid");
		if (!value || strcmp(value, ssid))
			continue;

		if (roam_uci_bool(uci_lookup_option_string(ctx, s, "disabled")))
			issues |= PAIR_DISABLED;

		if (!roam_uci_bool(uci_lookup_option_string(ctx, s, "ieee80211k")))
			issues |= PAIR_NO_11K;

		if (!roam_uci_bool(uci_lookup_option_string(ctx, s, "bss_transition")))
			issues |= PAIR_NO_11V;

		iface_read(ctx, s, &cur);

		if (!count++) {
			first = cur;
			continue;
		}

		if (!same_string(first.encryption, cur.encryption))
			issues |= PAIR_ENCRYPTION;

		if (!same_string(first.key, cur.key))
			issues |= PAIR_KEY;

		if (!same_string(first.network, cur.network))
			issues |= PAIR_NETWORK;

		if (first.ft != cur.ft)
			issues |= PAIR_FT;
		else if (first.ft && !same_string(first.mobility_domain, cur.mobility_domain))
			issues |= PAIR_FT;
	}

	if (count < 2)
		issues |= PAIR_NO_PEER;

	return issues;
}

void roam_pair_evaluate(void)
{
	struct uci_session u;
	struct roam_bss *bss;

	list_for_each_entry(bss, &roam_bss_list, list) {
		bss->pair_peer = roam_bss_target(bss);
		bss->pair_issues = roam_bss_matches(bss) ? 0 : PAIR_NOT_MANAGED;
	}

	if (!uci_session_open(&u, "wireless"))
		return;

	list_for_each_entry(bss, &roam_bss_list, list) {
		if (bss->pair_issues & PAIR_NOT_MANAGED)
			continue;

		if (!bss->pair_peer) {
			bss->pair_issues |= PAIR_NO_PEER;
			continue;
		}

		bss->pair_issues |= ssid_issues(u.ctx, u.pkg, bss->ssid);

		if (!bss->nr || !bss->pair_peer->nr)
			bss->pair_issues |= PAIR_NO_NEIGHBOR;
	}

	uci_session_close(&u);
}
