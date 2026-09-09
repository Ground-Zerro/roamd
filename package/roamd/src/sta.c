#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "roamd.h"
#include "mesh.h"

#define STA_FLAP_LIMIT	2
#define STA_FLAP_PAUSE	900000

struct avl_tree roam_sta_tree;
uint64_t roam_now;

void roam_time_update(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	roam_now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int sta_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, 6);
}

void roam_sta_setup(void)
{
	avl_init(&roam_sta_tree, sta_cmp, false, NULL);
}

struct roam_sta *roam_sta_get(const uint8_t *addr, bool create)
{
	struct roam_sta *sta;

	sta = avl_find_element(&roam_sta_tree, addr, sta, avl);
	if (sta || !create)
		return sta;

	sta = calloc(1, sizeof(*sta));
	if (!sta)
		return NULL;

	memcpy(sta->addr, addr, 6);
	snprintf(sta->mac, sizeof(sta->mac), "%02x:%02x:%02x:%02x:%02x:%02x",
		 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

	sta->avl.key = sta->addr;

	if (avl_insert(&roam_sta_tree, &sta->avl)) {
		free(sta);
		return NULL;
	}

	return sta;
}

static void roam_sta_delete(struct roam_sta *sta)
{
	avl_delete(&roam_sta_tree, &sta->avl);
	free(sta);
}

void roam_sta_expire(void)
{
	struct roam_sta *sta, *tmp;
	enum roam_band band;

	avl_for_each_element_safe(&roam_sta_tree, sta, avl, tmp) {
		uint64_t newest = 0;

		if (sta->bss)
			continue;

		for (band = 0; band < BAND_MAX; band++) {
			if (sta->band[band].seen > newest)
				newest = sta->band[band].seen;
		}

		if (roam_now - newest > config.age_time)
			roam_sta_delete(sta);
	}
}

void roam_sta_drop_bss(struct roam_bss *bss)
{
	struct roam_sta *sta;

	avl_for_each_element(&roam_sta_tree, sta, avl) {
		if (sta->bss == bss)
			roam_sta_disconnected(sta);
	}
}

static void sta_band_seen(struct roam_sta *sta, enum roam_band band, int signal)
{
	struct roam_sta_band *info = &sta->band[band];

	info->present = true;
	info->seen = roam_now;

	if (signal != ROAMD_NO_SIGNAL)
		info->signal = signal;
}

void roam_sta_event(struct roam_bss *bss, const uint8_t *addr, int signal)
{
	struct roam_sta *sta = roam_sta_get(addr, true);

	if (sta)
		sta_band_seen(sta, bss->band, signal);
}

void roam_sta_set_connected(struct roam_sta *sta, struct roam_bss *bss, int signal)
{
	if (sta->bss != bss) {
		struct roam_bss *prev = sta->bss;

		if (prev)
			mesh_log_local(sta->mac, prev->band, bss->band, MESH_EV_ROAM);
		else
			mesh_log_local(sta->mac, MESH_BAND_NA, bss->band, MESH_EV_CONNECT);

		if (prev && prev->band != bss->band) {
			if (sta->last_steer && bss->band == sta->steer_from &&
			    roam_now - sta->last_steer < config.age_time) {
				if (++sta->flap_count >= STA_FLAP_LIMIT) {
					sta->give_up_until = roam_now + STA_FLAP_PAUSE;
					roam_log(ROAM_L_INFO,
						 "roamd: %s keeps returning to %s GHz, leaving it alone",
						 sta->mac, roam_band_name(bss->band));
				}
			} else {
				sta->flap_count = 0;
				sta->give_up_until = 0;
			}
		}

		sta->bss = bss;
		sta->connected_since = roam_now;
		sta->steer_count = 0;
		sta->beacon_req_silent = 0;
		sta->btm_rejected = false;
		sta->kick_at = 0;
	}

	sta_band_seen(sta, bss->band, signal);
}

void roam_sta_reset(struct roam_sta *sta)
{
	sta->bss = NULL;
	sta->connected_since = 0;
	sta->kick_at = 0;
}

void roam_sta_disconnected(struct roam_sta *sta)
{
	if (sta->bss)
		mesh_log_local(sta->mac, sta->bss->band, MESH_BAND_NA, MESH_EV_DISCONNECT);

	roam_sta_reset(sta);
}
