#!/bin/sh
. /usr/share/libubox/jshn.sh

g() { uci -q get "roamd.$1"; }

ap_field() {
	local f="$1" sect
	for sect in $(uci -q show wireless | sed -n 's/^wireless\.\([^.]*\)=wifi-iface$/\1/p'); do
		case "$sect" in mesh_bh_*) continue;; esac
		[ "$(uci -q get "wireless.$sect.mode")" = "ap" ] || continue
		uci -q get "wireless.$sect.$f"
		return
	done
}

json_init
json_add_string controller_id "$(g mesh.controller_id)"

json_add_object wifi
for o in ssid encryption key; do
	v=$(ap_field "$o")
	[ -n "$v" ] && json_add_string "$o" "$v"
done
json_close_object

json_add_object credentials
rh=$(sed -n 's/^root:\([^:]*\):.*/\1/p' /etc/shadow 2>/dev/null)
case "$rh" in
	""|"*"|"!"|"!!"|"x") ;;
	*) json_add_string root_hash "$rh" ;;
esac
json_close_object

json_add_object global
for o in ssid mobility_domain band_steering prefer_band fast_transition ft_over_ds neighbor_reports bss_transition; do
	v=$(g "global.$o")
	[ -n "$v" ] && json_add_string "$o" "$v"
done
json_close_object

json_add_object policy
for o in rssi_good rssi_low rssi_diff kick_rssi cross_band_delta hold_time age_time \
	 check_time_low check_time_high poll_interval deny_time deny_probe allow_kick \
	 kick_delay steer_retries beacon_req_interval log_level; do
	v=$(g "policy.$o")
	[ -n "$v" ] && json_add_string "$o" "$v"
done
json_close_object

json_add_object backhaul
for o in backhaul_enabled backhaul_ssid backhaul_key ft_key wifi_shutdown; do
	v=$(g "mesh.$o")
	[ -n "$v" ] && json_add_string "$o" "$v"
done
bh_ssid=$(g mesh.backhaul_ssid)
if [ -n "$bh_ssid" ]; then
	bssids=$(iwinfo 2>/dev/null | awk -v want="\"$bh_ssid\"" '
		$2 == "ESSID:" && $3 == want { seen = 1; next }
		seen && $1 == "Access" && $2 == "Point:" { print $3; seen = 0 }' |
		tr '\n' ' ' | sed 's/ *$//')
	[ -n "$bssids" ] && json_add_string backhaul_bssids "$bssids"
fi
json_close_object

json_add_array devices
for sect in $(uci -q show roamd | sed -n 's/^roamd\.\(@device\[[0-9]*\]\|[a-z0-9_]*\)=device$/\1/p'); do
	mac=$(uci -q get "roamd.$sect.mac")
	[ -n "$mac" ] || continue
	json_add_object
	json_add_string mac "$mac"
	b=$(uci -q get "roamd.$sect.band"); [ -n "$b" ] && json_add_string band "$b"
	json_add_array nodes
	for nd in $(uci -q get "roamd.$sect.node" 2>/dev/null); do
		json_add_string "" "$nd"
	done
	json_close_array
	json_close_object
done
json_close_array

json_dump
