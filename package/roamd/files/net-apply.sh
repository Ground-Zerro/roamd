#!/bin/sh
BR="$1"
LOG="/tmp/roamd-diag.log"
BACKUP="/tmp/roamd-netrollback"

log() {
	echo "$(date '+%H:%M:%S') net-apply: $1" >> "$LOG"
}

addr_of() {
	ip -o -4 addr show dev "$BR" 2>/dev/null | awk '{ print $4 }' | head -1
}

unregister() {
	ubus call service delete '{"name":"roamd-netapply"}' >/dev/null 2>&1
}

old=$(addr_of)
log "starting, $BR was ${old:-none}"

/etc/init.d/dnsmasq stop >/dev/null 2>&1
/etc/init.d/odhcpd stop >/dev/null 2>&1
/etc/init.d/firewall restart >/dev/null 2>&1
/etc/init.d/network restart >/dev/null 2>&1

waited=0
while [ "$waited" -lt 120 ]; do
	sleep 5
	waited=$((waited + 5))
	now=$(addr_of)
	[ -n "$now" ] && [ "$now" != "$old" ] && {
		log "got $now after ${waited}s"
		unregister
		exit 0
	}
done

log "no new address on $BR after ${waited}s, rolling back"
cp "$BACKUP/network" "$BACKUP/dhcp" "$BACKUP/firewall" /etc/config/
/etc/init.d/dnsmasq enable >/dev/null 2>&1
/etc/init.d/odhcpd enable >/dev/null 2>&1
/etc/init.d/dnsmasq start >/dev/null 2>&1
/etc/init.d/odhcpd start >/dev/null 2>&1
/etc/init.d/firewall restart >/dev/null 2>&1
/etc/init.d/network restart >/dev/null 2>&1
unregister
