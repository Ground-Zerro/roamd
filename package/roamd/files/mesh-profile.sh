#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

g() { uci -q get "roamd.$1"; }

backhaul_parents() {
	local ssid="$1" sect id file conn via

	{
		iwinfo 2>/dev/null | awk -v want="\"$ssid\"" '
			$2 == "ESSID:" { seen = $3 == want; next }
			seen && $1 == "Access" && $2 == "Point:" { bssid = tolower($3) }
			seen && $1 == "Mode:" && $4 ~ /^[0-9]+$/ { print "B", bssid, "controller", ($4 > 14 ? "5" : "2.4"); seen = 0 }'

		for sect in $(member_sections); do
			id=$(uci -q get "roamd.$sect.id")
			file="$MEMBERS_DIR/$id.json"
			[ -n "$id" ] && [ "$(jsonfilter -e '@.online' < "$file" 2>/dev/null)" = true ] || continue

			conn=$(jsonfilter -e '@.connection' < "$file" 2>/dev/null)
			via=$(jsonfilter -e '@.via' < "$file" 2>/dev/null)
			echo "M $id ${conn:-wired} ${via:-controller}"
			jsonfilter -e "@.aps[@.ssid='$ssid']" < "$file" 2>/dev/null |
				sed -n "s/.*\"bssid\": \"\([^\"]*\)\".*\"band\": \"\([^\"]*\)\".*/B \1 $id \2/p"
		done
	} | awk '
		function resolve(id,   o) {
			if (id in depth)
				return depth[id]
			if ((id in busy) || !(id in conn))
				return -1
			busy[id] = 1

			if (conn[id] != "wifi") {
				depth[id] = 0
				chain[id] = id
				return 0
			}

			o = owner[via[id]]
			if (o == "controller") {
				depth[id] = 1
				chain[id] = id
				return 1
			}
			if (o == "" || resolve(o) < 0)
				return -1

			depth[id] = depth[o] + 1
			chain[id] = chain[o] "-" id
			return depth[id]
		}
		$1 == "B" { owner[$2] = $3; band[$2] = $4; order[++n] = $2; next }
		$1 == "M" { conn[$2] = $3; via[$2] = tolower($4) }
		END {
			for (i = 1; i <= n; i++) {
				b = order[i]
				o = owner[b]
				if (o == "controller")
					entry = b "/0//" band[b]
				else if (resolve(o) >= 0)
					entry = b "/" depth[o] "/" chain[o] "/" band[b]
				else
					continue
				out = out (out == "" ? "" : " ") entry
			}
			print out
		}'
}

GLOBAL_OPTS="ssid mobility_domain band_steering prefer_band fast_transition ft_over_ds
	neighbor_reports bss_transition"
POLICY_OPTS="rssi_good rssi_low rssi_diff kick_rssi cross_band_delta hold_time age_time
	check_time_low check_time_high poll_interval deny_time deny_probe allow_kick
	steer_retries beacon_req_interval log_level"

cfg_ready=""
if json_load "$(ubus -t 3 call roamd config 2>/dev/null)" 2>/dev/null; then
	cfg_ready=1
	for o in $GLOBAL_OPTS $POLICY_OPTS; do
		json_get_var v "$o"
		eval "cfg_$o=\"\$v\""
	done
fi

emit() {
	local v

	if [ -n "$cfg_ready" ]; then
		eval "v=\$cfg_$2"
		json_add_string "$2" "$v"
		return
	fi

	v=$(g "$1.$2")
	[ -n "$v" ] && json_add_string "$2" "$v"
}

ap_section_find

json_init
json_add_string controller_id "$(g mesh.controller_id)"

json_add_object wifi
for o in ssid encryption key; do
	v=$(ap_field "$o")
	[ -n "$v" ] && json_add_string "$o" "$v"
done
json_close_object

json_add_object system
for o in timezone zonename; do
	v=$(uci -q get "system.@system[0].$o")
	[ -n "$v" ] && json_add_string "$o" "$v"
done
json_close_object

json_add_object credentials
if grep -q '^root:' /etc/shadow 2>/dev/null; then
	rh=$(sed -n 's/^root:\([^:]*\):.*/\1/p' /etc/shadow)
	[ "$rh" = "x" ] || json_add_string root_hash "$rh"
fi
json_close_object

json_add_object global
for o in $GLOBAL_OPTS; do
	emit global "$o"
done
json_close_object

json_add_object policy
for o in $POLICY_OPTS; do
	emit policy "$o"
done
json_close_object

json_add_object backhaul
for o in backhaul_enabled backhaul_ssid backhaul_key ft_key wifi_shutdown backhaul_delta backhaul_min_signal; do
	v=$(g "mesh.$o")
	[ -n "$v" ] && json_add_string "$o" "$v"
done
bh_ssid=$(g mesh.backhaul_ssid)
if [ -n "$bh_ssid" ]; then
	parents=$(backhaul_parents "$bh_ssid")
	[ -n "$parents" ] && json_add_string backhaul_parents "$parents"
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
