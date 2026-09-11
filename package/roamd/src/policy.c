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

static struct blob_buf b;

static enum roam_band preferred_band(void)
{
	return config.prefer == PREFER_LOW ? BAND_LOW : BAND_HIGH;
}

static enum roam_band locked_band(enum roam_lock lock)
{
	return lock == LOCK_LOW ? BAND_LOW : BAND_HIGH;
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

static int estimated_signal(const struct roam_sta *sta, enum roam_band band, enum roam_band from)
{
	const struct roam_sta_band *info = &sta->band[band];

	if (band_info_fresh(info, band))
		return info->signal;

	if (sta->band[from].signal == ROAMD_NO_SIGNAL)
		return ROAMD_NO_SIGNAL;

	if (band == BAND_HIGH && from == BAND_LOW)
		return sta->band[from].signal - config.cross_band_delta;

	if (band == BAND_LOW && from == BAND_HIGH)
		return sta->band[from].signal + config.cross_band_delta;

	return ROAMD_NO_SIGNAL;
}

bool roam_policy_allow(struct roam_sta *sta, struct roam_bss *bss, enum roam_event ev)
{
	struct roam_sta_band *pref, *cur;
	enum roam_band want;
	enum roam_lock lock;

	if (!config.enabled)
		return true;

	if (!roam_device_node_allowed(sta->addr, mesh_self_node_id()))
		return false;

	if (ev == EVENT_ASSOC)
		return true;

	if (!roam_bss_matches(bss) || !roam_bss_target(bss))
		return true;

	lock = roam_device_lock(sta->addr);
	if (lock != LOCK_NONE)
		return bss->band == locked_band(lock);

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

static void policy_beacon_request(struct roam_sta *sta, struct roam_bss *from, struct roam_bss *to)
{
	if (!sta->rrm || !config.neighbor_reports || !to->op_class)
		return;

	if (sta->beacon_req_silent >= config.steer_retries)
		return;

	if (roam_now - sta->last_beacon_req < config.beacon_req_interval)
		return;

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

	mesh_log_local(sta->mac, from->band, to->band, MESH_EV_STEER);

	roam_log(ROAM_L_INFO, "roamd: steering %s from %s GHz to %s GHz (attempt %u of %u)",
		 sta->mac, roam_band_name(from->band), roam_band_name(to->band),
		 sta->steer_count, config.steer_retries);
}

void roam_policy_kick(struct roam_sta *sta, struct roam_bss *from)
{
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "addr", sta->mac);
	blobmsg_add_u32(&b, "reason", KICK_REASON);
	blobmsg_add_u8(&b, "deauth", 1);
	blobmsg_add_u32(&b, "ban_time", config.deny_time);
	roam_bss_invoke(from, "del_client", &b);

	roam_log(ROAM_L_INFO, "roamd: disconnecting %s from %s (%s GHz)",
		 sta->mac, from->ifname, roam_band_name(from->band));

	sta->last_steer = roam_now;
	sta->steer_from = from->band;
	sta->steer_count++;

	mesh_log_local(sta->mac, from->band, MESH_BAND_NA, MESH_EV_KICK);
	roam_sta_reset(sta);
}

bool roam_policy_can_steer(const struct roam_sta *sta)
{
	if (sta->steer_count >= config.steer_retries)
		return false;

	return !sta->last_steer || roam_now - sta->last_steer >= STEER_RETRY_INTERVAL;
}

static bool sta_should_leave(struct roam_sta *sta, struct roam_bss *bss,
			     struct roam_bss *target, enum roam_lock lock)
{
	enum roam_band want = preferred_band();
	int here = sta->band[bss->band].signal;
	int there;

	if (lock != LOCK_NONE)
		return bss->band != locked_band(lock);

	if (here != ROAMD_NO_SIGNAL && here < config.kick_rssi)
		return true;

	if (bss->band == want) {
		if (here == ROAMD_NO_SIGNAL || here >= config.rssi_low)
			return false;

		if (!band_info_fresh(&sta->band[target->band], target->band))
			return false;

		return sta->band[target->band].signal > here;
	}

	if (want == BAND_HIGH && !sta->band[bss->band].ht)
		return false;

	there = estimated_signal(sta, want, bss->band);
	if (there == ROAMD_NO_SIGNAL || there < config.rssi_good)
		return false;

	if (here != ROAMD_NO_SIGNAL && here - there >= config.rssi_diff)
		return false;

	return true;
}

void roam_policy_run(struct roam_bss *bss)
{
	struct roam_bss *target;
	struct roam_sta *sta;

	if (!config.enabled)
		return;

	avl_for_each_element(&roam_sta_tree, sta, avl) {
		if (sta->bss != bss)
			continue;

		if (!roam_device_node_allowed(sta->addr, mesh_self_node_id()))
			roam_policy_kick(sta, bss);
	}

	if (!config.bss_transition)
		return;

	target = roam_bss_target(bss);
	if (!target)
		return;

	avl_for_each_element(&roam_sta_tree, sta, avl) {
		enum roam_lock lock;

		if (sta->bss != bss)
			continue;

		lock = roam_device_lock(sta->addr);

		if (lock == LOCK_NONE && (!config.band_steering || config.prefer == PREFER_NONE))
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

		if (lock == LOCK_NONE && !band_info_fresh(&sta->band[target->band], target->band))
			policy_beacon_request(sta, bss, target);

		if (!sta_should_leave(sta, bss, target, lock))
			continue;

		if (!sta->btm) {
			if (config.allow_kick)
				roam_policy_kick(sta, bss);
			continue;
		}

		policy_btm(sta, bss, target);
	}
}
