#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

OUT="/var/run/roamd/candidates.json"
mkdir -p /var/run/roamd

pkg_state() {
	local branch="$1" arch="$2" rc

	[ -n "$branch" ] && [ -n "$arch" ] || { echo missing; return; }

	ensure_curl
	pkg_index_meta "$branch" "$arch" >/dev/null
	rc=$?
	[ "$rc" = 0 ] || [ -z "$(pkg_local "$branch" "$arch")" ] || rc=0

	case $rc in
		0) echo ready ;;
		1) echo missing ;;
		*) echo unreachable ;;
	esac
}

probe() {
	local addr="$1" mac="$2"
	local board hostname model board_name version role ctrl arch branch

	board=$(node_ssh "$addr" 'ubus call system board' 2>/dev/null)
	[ -n "$board" ] || return 1

	role=$(node_ssh "$addr" 'uci -q get roamd.mesh.role' 2>/dev/null)
	ctrl=$(node_ssh "$addr" 'uci -q get roamd.mesh.controller_id' 2>/dev/null)
	[ "$role" = "node" ] && [ -n "$ctrl" ] && return 1

	json_load "$board" 2>/dev/null || return 1
	json_get_var hostname hostname
	json_get_var model model
	json_get_var board_name board_name
	json_select release 2>/dev/null && json_get_var version version

	json_init
	json_add_string addr "$addr"
	json_add_string mac "$mac"
	json_add_string name "${hostname:-OpenWrt}"
	json_add_string model "${model:-}"
	json_add_string board "${board_name:-}"
	json_add_string os "${version:-}"

	branch="${version%.*}"
	arch=$(node_arch "$addr")
	json_add_string arch "${arch:-}"
	json_add_string pkg "$(pkg_state "$branch" "$arch")"
	json_dump | tr -d '\n\t'
}

self="$(own_addrs | tr '\n' ' ')"
dev=$(lan_device)

for sub in $(lan_subnets); do
	warm_subnet "$(subnet_base "$sub")"
done

: > /var/run/roamd/cand-items.txt
: > /var/run/roamd/cand-macs.txt

[ -n "$dev" ] && ip neigh show dev "$dev" | sort -u | while read -r addr kw mac rest; do
	[ "$kw" = "lladdr" ] || continue
	case "$addr" in *:*) continue;; esac
	case " $self " in *" $addr "*) continue;; esac
	case "$rest" in FAILED*|INCOMPLETE*) continue;; esac
	item=$(probe "$addr" "$mac") || continue
	printf '%s\n' "$item" >> /var/run/roamd/cand-items.txt
	printf '%s\n' "$mac" >> /var/run/roamd/cand-macs.txt
done

for scoped in $([ -n "$dev" ] && ll_neighbours "$dev"); do
	mac=$(ip -6 neigh show dev "$dev" | awk -v a="${scoped%%\%*}" '$1 == a { print $3; exit }')
	grep -qiF "$mac" /var/run/roamd/cand-macs.txt 2>/dev/null && continue
	item=$(probe "$scoped" "$mac") || continue
	printf '%s\n' "$item" >> /var/run/roamd/cand-items.txt
	printf '%s\n' "$mac" >> /var/run/roamd/cand-macs.txt
done

rm -f /var/run/roamd/cand-macs.txt

items=$(tr '\n' ',' < /var/run/roamd/cand-items.txt | sed 's/,*$//')
rm -f /var/run/roamd/cand-items.txt
printf '{"candidates":[%s]}' "$items" > "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
cat "$OUT"
