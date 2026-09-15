#!/bin/sh
BR="$1"
CONTROLLER="$2"
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

log "starting, $BR was $(addr_of)"

/etc/init.d/dnsmasq stop >/dev/null 2>&1
/etc/init.d/odhcpd stop >/dev/null 2>&1
ln -sf /tmp/resolv.conf.d/resolv.conf.auto /tmp/resolv.conf
/etc/init.d/firewall restart >/dev/null 2>&1
/etc/init.d/network restart >/dev/null 2>&1

waited=0
while [ "$waited" -lt 120 ]; do
	sleep 5
	waited=$((waited + 5))
	ping -c 1 -W 2 "$CONTROLLER" >/dev/null 2>&1 || continue

	log "controller $CONTROLLER answers to $(addr_of) after ${waited}s"
	unregister
	exit 0
done

log "controller $CONTROLLER does not answer after ${waited}s, rolling back"
cp "$BACKUP/network" "$BACKUP/dhcp" "$BACKUP/firewall" /etc/config/
/etc/init.d/dnsmasq enable >/dev/null 2>&1
/etc/init.d/odhcpd enable >/dev/null 2>&1
/etc/init.d/dnsmasq start >/dev/null 2>&1
/etc/init.d/odhcpd start >/dev/null 2>&1
/etc/init.d/firewall restart >/dev/null 2>&1
/etc/init.d/network restart >/dev/null 2>&1
unregister
