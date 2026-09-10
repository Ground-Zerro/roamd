#!/bin/sh

[ "$(uci -q get roamd.mesh.role)" = "node" ] || exit 0

caddr=$(uci -q get roamd.mesh.controller_addr)
changed=""
fixed=""

set_opt() {
	[ "$(uci -q get "$1")" = "$2" ] && return 0
	uci -q set "$1=$2" && changed="$changed ${1#dhcp.}"
}

set_opt dhcp.lan.ignore 1
set_opt dhcp.lan.dhcpv4 disabled
set_opt dhcp.lan.dhcpv6 disabled
set_opt dhcp.lan.ra disabled
set_opt dhcp.@dnsmasq[0].noresolv 1

if [ -n "$caddr" ] && [ "$(uci -q get dhcp.@dnsmasq[0].server)" != "$caddr" ]; then
	uci -q delete dhcp.@dnsmasq[0].server
	uci -q add_list dhcp.@dnsmasq[0].server="$caddr"
	changed="$changed forwarder=$caddr"
fi

[ -n "$changed" ] && uci commit dhcp

for svc in dnsmasq odhcpd; do
	[ -x "/etc/init.d/$svc" ] || continue

	if "/etc/init.d/$svc" enabled 2>/dev/null; then
		"/etc/init.d/$svc" disable >/dev/null 2>&1
		fixed="$fixed $svc:autostart-off"
	fi

	if "/etc/init.d/$svc" running 2>/dev/null; then
		"/etc/init.d/$svc" stop >/dev/null 2>&1
		fixed="$fixed $svc:stopped"
	fi
done

[ -n "$changed$fixed" ] && logger -t roamd "mesh: dumb AP enforced —$changed$fixed"

exit 0
