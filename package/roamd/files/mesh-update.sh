#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

ADDR="$1"
TASK="$2"
STATE="/var/run/roamd/acquire/${TASK}"

acquire_dir_trim
report() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$STATE"; }
fail() { report "$1" error "$2"; exit 1; }

: > "$STATE"

node_forget "$ADDR"

board=$(node_ssh "$ADDR" 'ubus call system board' 2>/dev/null)
[ -n "$board" ] || fail probe "node is unreachable"
json_load "$board" 2>/dev/null || fail probe "cannot read device info"
json_select release 2>/dev/null && json_get_var version version

branch="${version%.*}"
[ -n "$branch" ] || fail compat "unknown OpenWrt version"

report update progress "updating roamd for $branch"
member_id=""
for sect in $(uci -q show roamd | sed -n 's/^roamd\.\(@member\[[0-9]*\]\|[a-z0-9_]*\)=member$/\1/p'); do
	[ "$(uci -q get roamd.$sect.addr)" = "$ADDR" ] && member_id=$(uci -q get roamd.$sect.id)
done

pkg_source() {
	[ -n "$member_id" ] || return 0
	mkdir -p /var/run/roamd/members
	printf '%s' "$1" > "/var/run/roamd/members/${member_id}.pkgsrc"
}

install_roamd_pkg "$ADDR" "$branch" force
case $? in
	0) pkg_source repository ;;
	4) pkg_source cache; report update progress "repository unreachable, installed from the controller cache" ;;
	2) fail compat "no package for OpenWrt $branch" ;;
	3) fail update "package source is not trusted: pkg_url must be https and /etc/roamd/pkg.crt must exist" ;;
	*) fail update "opkg install failed on the node" ;;
esac

node_ssh "$ADDR" '/etc/init.d/roamd restart >/dev/null 2>&1'
report done ok "updated"
