#!/bin/sh
. /usr/libexec/roamd/mesh-lib.sh

ID="$1"
TASK="$2"
STATE="${ACQUIRE_DIR}/${TASK}"

acquire_dir_trim

report() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$STATE"; }

member_addr() {
	local sect
	for sect in $(uci -q show roamd | sed -n 's/^roamd\.\(@member\[[0-9]*\]\|[a-z0-9_]*\)=member$/\1/p'); do
		[ "$(uci -q get roamd.$sect.id)" = "$ID" ] && uci -q get roamd.$sect.addr && return 0
	done
	return 1
}

drop_member() {
	ubus -t 5 call roamd mesh_member_remove "{\"id\":\"$ID\"}" >/dev/null 2>&1
	node_forget "$ADDR"
	report done ok "$ID"
}

ADDR=$(member_addr)
[ -n "$ADDR" ] || { report reset error "the node is not in the system"; report done ok "$ID"; exit 0; }

report reset progress "resetting the node to factory settings"

if ! node_ssh "$ADDR" "
	cat > /tmp/roamd-release.sh <<'EOS'
#!/bin/sh
sleep 2
firstboot -y >/dev/null 2>&1
sync
reboot
EOS
	ubus call service add \"{\\\"name\\\":\\\"roamd-release\\\",\\\"instances\\\":{\\\"main\\\":{\\\"command\\\":[\\\"/bin/sh\\\",\\\"/tmp/roamd-release.sh\\\"]}}}\" >/dev/null 2>&1
	echo accepted
" | grep -q accepted; then
	report reset error "the node did not accept the reset, release it manually"
	drop_member
	exit 0
fi

waited=0
while [ "$waited" -lt 90 ]; do
	sleep 5
	waited=$((waited + 5))
	node_ssh "$ADDR" 'exit 0' >/dev/null 2>&1 || {
		report reset ok "the node is resetting to factory settings"
		drop_member
		exit 0
	}
done

report reset error "the node is still reachable, the reset may have failed"
drop_member
