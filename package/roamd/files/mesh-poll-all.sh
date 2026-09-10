#!/bin/sh
. /usr/libexec/roamd/mesh-lib.sh
. /usr/libexec/roamd/mesh-rpc.sh
. /usr/share/libubox/jshn.sh

mode="${1:-monitor}"
mkdir -p "$MEMBERS_DIR"

profile=$(/usr/libexec/roamd/mesh-profile.sh)

ctrl_ap_field() {
	local f="$1" sect
	for sect in $(uci -q show wireless | sed -n 's/^wireless\.\([^.]*\)=wifi-iface$/\1/p'); do
		case "$sect" in mesh_bh_*) continue;; esac
		[ "$(uci -q get "wireless.$sect.mode")" = "ap" ] || continue
		uci -q get "wireless.$sect.$f"
		return
	done
}

c_ssid=$(ctrl_ap_field ssid)
c_enc=$(ctrl_ap_field encryption)
c_md=$(ctrl_ap_field mobility_domain)
c_ft=$(ctrl_ap_field ieee80211r)
c_bh=$(uci -q get roamd.mesh.backhaul_ssid)

for sect in $(member_sections); do
	id=$(uci -q get "roamd.$sect.id")
	addr=$(uci -q get "roamd.$sect.addr")
	managed=$(uci -q get "roamd.$sect.managed")
	[ -n "$id" ] && [ -n "$addr" ] || continue

	was_online=$(jsonfilter -e '@.online' < "$MEMBERS_DIR/$id.json" 2>/dev/null)

	session=$(rpc_login "$addr" "$id")
	if [ -z "$session" ]; then
		printf '{"online":false}' > "$MEMBERS_DIR/$id.json"
		continue
	fi

	if [ "$managed" != "0" ] && { [ "$mode" = "force" ] || [ "$was_online" != "true" ]; }; then
		rpc_call "$addr" "$session" roamd mesh_apply "$profile" "$id" >/dev/null 2>&1
	fi

	report=$(rpc_call "$addr" "$session" roamd mesh_report '{}' "$id" | jsonfilter -e '@.result[1]' 2>/dev/null)
	if [ -n "$report" ]; then
		node_ver=$(echo "$report" | jsonfilter -e '@.pkg_version' 2>/dev/null)
		node_ui=$(echo "$report" | jsonfilter -e '@.ui_version' 2>/dev/null)
		os=$(echo "$report" | jsonfilter -e '@.os_version' 2>/dev/null)
		arch=$(echo "$report" | jsonfilter -e '@.arch' 2>/dev/null)
		target=$(pkg_version "${os%.*}" "$arch" 2>/dev/null)
		target_ui=$(pkg_version "${os%.*}" "$arch" luci-app-roamd 2>/dev/null)
		if [ -n "$target" ] && { sys_newer "$target" "$node_ver" ||
			{ [ -n "$node_ui" ] && [ -n "$target_ui" ] && sys_newer "$target_ui" "$node_ui"; }; }; then
			upd=true
		else
			upd=false
		fi

		n_ssid=$(echo "$report" | jsonfilter -e '@.profile.ssid' 2>/dev/null)
		n_enc=$(echo "$report" | jsonfilter -e '@.profile.encryption' 2>/dev/null)
		n_md=$(echo "$report" | jsonfilter -e '@.profile.mobility_domain' 2>/dev/null)
		n_ft=$(echo "$report" | jsonfilter -e '@.profile.ieee80211r' 2>/dev/null)
		n_bh=$(echo "$report" | jsonfilter -e '@.profile.backhaul_ssid' 2>/dev/null)

		issues=""
		add_issue() { issues="$issues${issues:+,}\"$1\""; }
		[ -n "$c_ssid" ] && [ "$c_ssid" != "$n_ssid" ] && add_issue ssid
		[ -n "$c_enc" ] && [ "$c_enc" != "$n_enc" ] && add_issue encryption
		[ -n "$c_md" ] && [ "$c_md" != "$n_md" ] && add_issue mobility_domain
		[ -n "$c_ft" ] && [ "$c_ft" != "$n_ft" ] && add_issue ieee80211r
		[ -n "$c_bh" ] && [ "$c_bh" != "$n_bh" ] && add_issue backhaul

		case "$issues" in
			*ssid*|*encryption*) cons=broken ;;
			"") cons=ok ;;
			*) cons=degraded ;;
		esac

		src=$(cat "$MEMBERS_DIR/${id}.pkgsrc" 2>/dev/null)
		echo "$report" | sed "s/}\$/,\"online\":true,\"update_available\":$upd,\"pkg_source\":\"$src\",\"consistency\":\"$cons\",\"issues\":[$issues]}/" > "$MEMBERS_DIR/$id.json"

		name=$(uci -q get "roamd.$sect.name")
		events=$(echo "$report" | jsonfilter -e '@.events' 2>/dev/null)
		if [ -n "$name" ] && [ -n "$events" ] && [ "$events" != "[ ]" ]; then
			ubus -t 3 call roamd mesh_log_ingest "{\"node\":\"$name\",\"events\":$events}" >/dev/null 2>&1
		fi
	else
		printf '{"online":false}' > "$MEMBERS_DIR/$id.json"
	fi
done

steer_diff=$(uci -q get roamd.policy.rssi_diff); steer_diff=${steer_diff:-30}
seen=/tmp/roamd-steer.$$
: > "$seen"

collect_seen() {
	local file="$1" id="$2" addr="$3" list mac sig i

	for list in clients heard; do
		i=0
		while :; do
			mac=$(jsonfilter -e "@.$list[$i].mac" < "$file" 2>/dev/null)
			[ -n "$mac" ] || break
			sig=$(jsonfilter -e "@.$list[$i].signal" < "$file" 2>/dev/null)
			i=$((i + 1))
			[ -n "$sig" ] || continue
			printf '%s %s %s %s %s\n' "$mac" "$id" "$addr" "$sig" "$list" >> "$seen"
		done
	done
}

self_report=/tmp/roamd-self.$$
ubus call roamd mesh_report > "$self_report" 2>/dev/null &&
	collect_seen "$self_report" controller local
rm -f "$self_report"

for sect in $(member_sections); do
	id=$(uci -q get "roamd.$sect.id")
	addr=$(uci -q get "roamd.$sect.addr")
	[ -n "$id" ] && [ -f "$MEMBERS_DIR/$id.json" ] || continue

	collect_seen "$MEMBERS_DIR/$id.json" "$id" "$addr"
done

node_in() { local a; for a in $2; do [ "$a" = "$1" ] && return 0; done; return 1; }

allowed_nodes_for() {
	local want="$1" sect m
	for sect in $(uci -q show roamd | sed -n 's/^roamd\.\(@device\[[0-9]*\]\|[a-z0-9_]*\)=device$/\1/p'); do
		m=$(uci -q get "roamd.$sect.mac" | tr 'A-Z' 'a-z')
		[ "$m" = "$want" ] && { uci -q get "roamd.$sect.node"; return; }
	done
}

for mac in $(awk '{print $1}' "$seen" | sort -u); do
	allow=$(allowed_nodes_for "$(echo "$mac" | tr 'A-Z' 'a-z')")

	best=$(grep "^$mac " "$seen" | while read -r m cid caddr csig ckind; do
		{ [ -z "$allow" ] || node_in "$cid" "$allow"; } && printf '%s %s %s %s\n' "$m" "$cid" "$caddr" "$csig"
	done | sort -k4 -n | tail -1)
	[ -n "$best" ] || continue
	best_id=$(echo "$best" | awk '{print $2}')
	best_sig=$(echo "$best" | awk '{print $4}')

	neigh=""; j=0
	while :; do
		bj=$(jsonfilter -e "@.aps[$j].bssid" < "$MEMBERS_DIR/$best_id.json" 2>/dev/null)
		[ -n "$bj" ] || break
		nr=$(jsonfilter -e "@.aps[$j].nr" < "$MEMBERS_DIR/$best_id.json" 2>/dev/null)
		[ -n "$nr" ] && neigh="$neigh${neigh:+,}\"$nr\""
		j=$((j + 1))
	done

	grep "^$mac " "$seen" | while read -r m cid caddr csig ckind; do
		[ "$ckind" = "clients" ] || continue
		[ "$cid" = "$best_id" ] && continue
		if [ -n "$allow" ] && ! node_in "$cid" "$allow"; then
			:
		elif [ $((best_sig - csig)) -lt "$steer_diff" ]; then
			continue
		fi
		if [ -n "$neigh" ]; then
			params="{\"mac\":\"$m\",\"neighbors\":[$neigh]}"
		else
			params="{\"mac\":\"$m\"}"
		fi
		if [ "$cid" = "controller" ]; then
			ubus -t 3 call roamd mesh_steer "$params" >/dev/null 2>&1
			continue
		fi

		session=$(rpc_login "$caddr" "$cid")
		[ -n "$session" ] && rpc_call "$caddr" "$session" roamd mesh_steer "$params" "$cid" >/dev/null 2>&1
	done
done

rm -f "$seen"

ms=/tmp/roamd-ms.$$
ubus -t 3 call roamd mesh_status > "$ms" 2>/dev/null

nb=0
json_init
json_add_array neighbors
add_aps() {
	local file="$1" path="$2" i=0 b s nr
	while :; do
		b=$(jsonfilter -e "@.${path}[$i].bssid" < "$file" 2>/dev/null)
		[ -n "$b" ] || break
		nr=$(jsonfilter -e "@.${path}[$i].nr" < "$file" 2>/dev/null)
		if [ -n "$nr" ]; then
			s=$(jsonfilter -e "@.${path}[$i].ssid" < "$file" 2>/dev/null)
			json_add_object ""
			json_add_string bssid "$b"
			json_add_string ssid "$s"
			json_add_string nr "$nr"
			json_close_object
			nb=$((nb + 1))
		fi
		i=$((i + 1))
	done
}

add_aps "$ms" self_aps
for sect in $(member_sections); do
	id=$(uci -q get "roamd.$sect.id")
	[ -n "$id" ] && [ -f "$MEMBERS_DIR/$id.json" ] && add_aps "$MEMBERS_DIR/$id.json" aps
done
json_close_array
payload=$(json_dump)
rm -f "$ms"

if [ "$nb" -gt 0 ]; then
	ubus -t 3 call roamd mesh_neighbors "$payload" >/dev/null 2>&1
	for sect in $(member_sections); do
		id=$(uci -q get "roamd.$sect.id")
		addr=$(uci -q get "roamd.$sect.addr")
		[ -n "$id" ] && [ -n "$addr" ] || continue
		session=$(rpc_login "$addr" "$id")
		[ -n "$session" ] && rpc_call "$addr" "$session" roamd mesh_neighbors "$payload" "$id" >/dev/null 2>&1
	done
fi
