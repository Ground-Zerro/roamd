#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roamd.h"
#include "mesh.h"

#define BTM_VALIDITY_PERIOD	100
#define BTM_MBO_REASON		5
#define BTM_CELL_PREF		0
#define KICK_REASON		5
#define STEER_RETRY_INTERVAL	5000
#define BEACON_REQ_PER_POLL	4

static struct blob_buf b;

static enum roam_band preferred_band(void)
{
	return config.prefer == PREFER_LOW ? BAND_LOW : BAND_HIGH;
}

static const char *bss_nr_string(const struct roam_bss *bss)
{
	static const struct blobmsg_policy policy[3] = {
		{ .type = BLOBMSG_TYPE_STRING },
		{ .type = BLOBMSG_TYPE_STRING },
		{ .type = BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *tb[3];

	if (!bss->nr)
		return NULL;

	blobmsg_parse_array(policy, 3, tb, blobmsg_data(bss->nr), blobmsg_data_len(bss->nr));
	if (!tb[2])
		return NULL;

	return blobmsg_get_string(tb[2]);
}

static bool band_info_fresh(const struct roam_sta_band *info, enum roam_band band)
{
	if (!info->present || info->signal == ROAMD_NO_SIGNAL)
		return false;

	return roam_now - info->seen <= config.age_time;
}

bool roam_policy_allow(struct roam_sta *sta, struct roam_bss *bss, enum roam_event ev)
{
	struct roam_sta_band *pref, *cur;
	enum roam_band want;

	if (!config.enabled || !roam_bss_matches(bss))
		return true;

	if (roam_admit(sta->addr, mesh_self_node_id(), bss->band) != ADMIT_OK)
		return false;

	if (ev == EVENT_ASSOC || !roam_bss_target(bss))
		return true;

	if (!config.band_steering || config.prefer == PREFER_NONE || !config.deny_probe)
		return true;

	want = preferred_band();
	if (bss->band == want)
		return true;

	pref = &sta->band[want];
	cur = &sta->band[bss->band];

	if (roam_now < cur->allow_until)
		return true;

	if (!pref->present || roam_now - pref->seen > config.check_time[want])
		goto allow;

	if (pref->signal == ROAMD_NO_SIGNAL || pref->signal < config.rssi_low)
		goto allow;

	if (pref->signal < config.rssi_good)
		goto allow;

	if (cur->signal != ROAMD_NO_SIGNAL && cur->signal - pref->signal >= config.rssi_diff)
		goto allow;

	if (!cur->deny_start)
		cur->deny_start = roam_now;

	if (roam_now - cur->deny_start < config.deny_time)
		return false;

	cur->deny_start = 0;
	cur->allow_until = roam_now + config.age_time;

	roam_log(ROAM_L_INFO, "roamd: %s insists on %s GHz, giving up",
		 sta->mac, roam_band_name(bss->band));

	return true;

allow:
	cur->deny_start = 0;
	return true;
}

static void policy_beacon_request(struct roam_sta *sta, struct roam_bss *from, struct roam_bss *to,
				  unsigned int *budget)
{
	if (!sta->rrm || !config.neighbor_reports || !to->op_class)
		return;

	if (sta->beacon_req_silent >= config.steer_retries)
		return;

	if (roam_now - sta->last_beacon_req < config.beacon_req_interval)
		return;

	if (!*budget)
		return;

	(*budget)--;

	sta->last_beacon_req = roam_now;
	sta->beacon_req_silent++;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "addr", sta->mac);
	blobmsg_add_string(&b, "ssid", to->ssid);
	blobmsg_add_u32(&b, "mode", 1);
	blobmsg_add_u32(&b, "duration", 50);
	blobmsg_add_u32(&b, "channel", to->channel);
	blobmsg_add_u32(&b, "op_class", to->op_class);
	roam_bss_invoke(from, "rrm_beacon_req", &b);
}

static void policy_btm(struct roam_sta *sta, struct roam_bss *from, struct roam_bss *to)
{
	const char *nr = bss_nr_string(to);
	void *list;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "addr", sta->mac);
	blobmsg_add_u32(&b, "dialog_token", ++sta->dialog_token);
	blobmsg_add_u8(&b, "disassociation_imminent", 0);
	blobmsg_add_u32(&b, "disassociation_timer", 0);
	blobmsg_add_u32(&b, "reassoc_delay", 0);

	blobmsg_add_u8(&b, "abridged", 1);
	blobmsg_add_u32(&b, "validity_period", BTM_VALIDITY_PERIOD);
	blobmsg_add_u32(&b, "mbo_reason", BTM_MBO_REASON);
	blobmsg_add_u32(&b, "cell_pref", BTM_CELL_PREF);

	list = blobmsg_open_array(&b, "neighbors");
	if (nr)
		blobmsg_add_string(&b, NULL, nr);
	blobmsg_close_array(&b, list);

	roam_bss_invoke(from, "bss_transition_request", &b);

	sta->last_steer = roam_now;
	sta->steer_from = from->band;
	sta->steer_count++;

	mesh_log_local(sta->mac, from->band, to->band, NULL, MESH_EV_STEER);

	roam_log(ROAM_L_INFO, "roamd: steering %s from %s GHz to %s GHz (attempt %u of %u)",
		 sta->mac, roam_band_name(from->band), roam_band_name(to->band),
		 sta->steer_count, config.steer_retries);
}

static void sta_del_client(struct roam_sta *sta, struct roam_bss *from)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "addr", sta->mac);
	blobmsg_add_u32(&b, "reason", KICK_REASON);
	blobmsg_add_u8(&b, "deauth", 1);
	blobmsg_add_u32(&b, "ban_time", config.deny_time);
	roam_bss_invoke(from, "del_client", &b);

	roam_sta_reset(sta);
}

static void policy_evict(struct roam_sta *sta, struct roam_bss *from, enum roam_admit verdict)
{
	bool node = verdict == ADMIT_DENY_NODE;

	roam_log(ROAM_L_INFO, "roamd: %s is not allowed %s, disconnecting from %s (%s GHz)",
		 sta->mac, node ? "on this device" : "on this band",
		 from->ifname, roam_band_name(from->band));

	mesh_log_local(sta->mac, from->band, MESH_BAND_NA, NULL,
		       node ? MESH_EV_DENY_NODE : MESH_EV_DENY_BAND);

	sta_del_client(sta, from);
}

void roam_policy_kick(struct roam_sta *sta, struct roam_bss *from)
{
	roam_log(ROAM_L_INFO, "roamd: disconnecting %s from %s (%s GHz)",
		 sta->mac, from->ifname, roam_band_name(from->band));

	sta->last_steer = roam_now;
	sta->steer_from = from->band;
	sta->steer_count++;

	mesh_log_local(sta->mac, from->band, MESH_BAND_NA, NULL, MESH_EV_KICK);
	sta_del_client(sta, from);
}

bool roam_policy_can_steer(const struct roam_sta *sta)
{
	if (sta->steer_count >= config.steer_retries)
		return false;

	return !sta->last_steer || roam_now - sta->last_steer >= STEER_RETRY_INTERVAL;
}

static bool sta_should_leave(struct roam_sta *sta, struct roam_bss *bss,
			     struct roam_bss *target)
{
	enum roam_band want = preferred_band();
	int here = sta->band[bss->band].signal;
	int there;

	if (here != ROAMD_NO_SIGNAL && here < config.kick_rssi)
		return true;

	if (!band_info_fresh(&sta->band[target->band], target->band))
		return false;

	there = sta->band[target->band].signal;

	if (here != ROAMD_NO_SIGNAL && there - here >= config.rssi_diff)
		return true;

	if (bss->band == want) {
		if (here == ROAMD_NO_SIGNAL || here >= config.rssi_low)
			return false;

		return there > here;
	}

	if (want == BAND_HIGH && !sta->band[bss->band].ht)
		return false;

	if (there < config.rssi_good)
		return false;

	return here == ROAMD_NO_SIGNAL || here - there < config.rssi_diff;
}

void roam_policy_run(struct roam_bss *bss)
{
	unsigned int beacon_budget = BEACON_REQ_PER_POLL;
	struct roam_bss *target;
	struct roam_sta *sta;

	if (!config.enabled)
		return;

	avl_for_each_element(&roam_sta_tree, sta, avl) {
		enum roam_admit verdict;

		if (sta->bss != bss)
			continue;

		verdict = roam_admit(sta->addr, mesh_self_node_id(), bss->band);
		if (verdict != ADMIT_OK)
			policy_evict(sta, bss, verdict);
	}

	if (!config.bss_transition || !config.band_steering || config.prefer == PREFER_NONE)
		return;

	target = roam_bss_target(bss);
	if (!target)
		return;

	avl_for_each_element(&roam_sta_tree, sta, avl) {
		if (sta->bss != bss)
			continue;

		if (roam_admit(sta->addr, mesh_self_node_id(), target->band) != ADMIT_OK)
			continue;

		if (roam_now - sta->connected_since < config.hold_time)
			continue;

		if (sta->give_up_until) {
			if (roam_now < sta->give_up_until)
				continue;

			sta->give_up_until = 0;
		}

		if (!roam_policy_can_steer(sta))
			continue;

		if (!band_info_fresh(&sta->band[target->band], target->band))
			policy_beacon_request(sta, bss, target, &beacon_budget);

		if (!sta_should_leave(sta, bss, target))
			continue;

		if (!sta->btm) {
			if (config.allow_kick)
				roam_policy_kick(sta, bss);
			continue;
		}

		policy_btm(sta, bss, target);
	}
}
