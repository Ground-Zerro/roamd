#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

ADDR="$1"
TASK="$2"

task_open "$TASK"



controller_addr_for() {
	local target="$1" net3="${1%.*}"

	if addr_is_v6 "$target"; then
		own_addrs | head -1
		return 0
	fi

	ip -4 -o addr show scope global | while read -r _ _ _ cidr _; do
		local ip="${cidr%/*}"
		[ "${ip%.*}" = "$net3" ] && { echo "$ip"; break; }
	done
}

register_member() {
	local id="$1" mac="$2" addr="$3" hostname="$4" sect found=""

	for sect in $(member_sections); do
		[ "$(uci -q get roamd.$sect.mac)" = "$mac" ] && found="$sect"
	done

	[ -n "$found" ] || found=$(uci add roamd member)
	uci set roamd.$found.id="$id"
	uci set roamd.$found.mac="$mac"
	uci set roamd.$found.addr="$addr"
	uci set roamd.$found.hostname="$hostname"
	[ -n "$(uci -q get roamd.$found.name)" ] || uci set roamd.$found.name="$hostname"
	uci set roamd.$found.managed='1'
	uci commit roamd
}


ensure_ssh_key
node_forget "$ADDR"

board=$(node_ssh "$ADDR" 'ubus call system board' 2>/dev/null)
[ -n "$board" ] || fail probe "SSH root without a password is not available"
json_load "$board" 2>/dev/null || fail probe "cannot read device info"
json_get_var hostname hostname
json_select release 2>/dev/null && json_get_var version version
json_select .. 2>/dev/null
report probe ok "${hostname:-OpenWrt} ${version}"

if addr_is_v6 "$ADDR"; then
	dev_probe="ip -6 -o addr show scope link 2>/dev/null | awk -v a='${ADDR%%\%*}/' 'index(\$4, a) == 1 { print \$2; exit }'"
else
	dev_probe="ip -o -4 addr show 2>/dev/null | awk -v a='$ADDR/' 'index(\$4, a) == 1 { print \$2; exit }'"
fi

mac=$(node_ssh "$ADDR" "
	dev=\$($dev_probe)
	cat /sys/class/net/\$dev/address 2>/dev/null")
mac=$(echo "$mac" | tr -d '\r\n ')
[ -n "$mac" ] && printf '%s\n' "$mac" > "${TASK_STATE}.mac"

branch="${version%.*}"
[ -n "$branch" ] || fail compat "unknown OpenWrt version"

report install progress "installing roamd and interface for $branch"
install_roamd_pkg "$ADDR" "$branch" force
case $? in
	0) : ;;
	4) report install progress "repository unreachable, installed from the controller cache" ;;
	2) fail compat "no package for OpenWrt $branch and no feed access" ;;
	3) fail install "the package repository must be https (pkg_url)" ;;
	*) fail install "package installation failed on the node" ;;
esac

cid=$(uci -q get roamd.mesh.controller_id)
cname=$(uci -q get system.@system[0].hostname); cname="${cname:-$(uname -n)}"
caddr=$(controller_addr_for "$ADDR")
member_id=$(echo "$mac" | tr -d ':' | tail -c 7)

report enroll progress "assigning node role"
node_ssh "$ADDR" "
	uci -q get roamd.mesh >/dev/null || uci set roamd.mesh=mesh

	for sect in \$(uci show wireless 2>/dev/null | sed -n 's/^wireless\.\(mesh_bh_[a-z0-9_]*\)=wifi-iface\$/\1/p'); do
		uci -q delete wireless.\$sect
	done
	uci -q commit wireless
	uci -q delete roamd.mesh.backhaul_ssid
	uci -q delete roamd.mesh.backhaul_key

	uci set roamd.mesh.role='node'
	uci set roamd.mesh.controller_id='$cid'
	uci set roamd.mesh.controller_name='$cname'
	uci set roamd.mesh.controller_addr='$caddr'
	uci set roamd.mesh.member_id='$member_id'
	uci commit roamd
	/etc/init.d/rpcd reload >/dev/null 2>&1
	/etc/init.d/roamd enable >/dev/null 2>&1
	/etc/init.d/roamd restart >/dev/null 2>&1
" || fail enroll "cannot assign the node role"

report network progress "switching the node to the controller subnet"

if addr_is_v6 "$ADDR"; then
	uplink_probe="ip -6 -o addr show scope link 2>/dev/null | awk -v a='${ADDR%%\%*}/' 'index(\$4, a) == 1 { print \$2; exit }'"
else
	uplink_probe="ip -o -4 addr show 2>/dev/null | awk -v a='$ADDR/' 'index(\$4, a) == 1 { print \$2; exit }'"
fi

node_ssh_long "$ADDR" "
	uplink=\$($uplink_probe)
	[ -n \"\$uplink\" ] || exit 1

	br=''
	for dev in \$(uci show network | sed -n 's/^network\\.\\(@device\\[[0-9]*\\]\\|[a-z0-9_]*\\)=device\$/\\1/p'); do
		[ \"\$(uci -q get network.\$dev.type)\" = 'bridge' ] || continue
		uci set network.\$dev.stp='1'
		[ -n \"\$br\" ] || br=\$dev
	done
	[ -n \"\$br\" ] || exit 2

	conduits=\$(ip -o link 2>/dev/null | sed -n 's/^[0-9]*: [^@]*@\([^:]*\):.*/\1/p' | sort -u)
	brname=\$(uci -q get network.\$br.name)
	skip=0

	[ \"\$uplink\" = \"\$brname\" ] && skip=1
	for c in \$conduits; do
		[ \"\$uplink\" = \"\$c\" ] && skip=1
	done
	case \" \$(uci -q get network.\$br.ports) \" in
		*\" \$uplink \"*) skip=1 ;;
	esac

	[ \"\$skip\" = 1 ] || uci add_list network.\$br.ports=\"\$uplink\"
	uci set network.\$br.macaddr='$mac'

	uci set network.lan.proto='dhcp'
	uci -q delete network.lan.ipaddr
	uci -q delete network.lan.netmask
	uci -q delete network.lan.gateway
	uci -q delete network.lan.ip6assign
	uci -q delete network.wan
	uci -q delete network.wan6

	uci set dhcp.lan.ignore='1'
	uci set dhcp.lan.dhcpv4='disabled'
	uci set dhcp.lan.dhcpv6='disabled'
	uci set dhcp.lan.ra='disabled'

	uci set dhcp.@dnsmasq[0].noresolv='1'
	uci -q delete dhcp.@dnsmasq[0].server
	uci add_list dhcp.@dnsmasq[0].server='$caddr'

	uci -q delete firewall.roamd_mesh
	for sect in \$(uci show firewall | sed -n 's/^firewall\\.\\(@forwarding\\[[0-9]*\\]\\)=forwarding\$/\\1/p' | sed '1!G;h;\$!d'); do
		uci delete firewall.\$sect
	done
	for sect in \$(uci show firewall | sed -n 's/^firewall\\.\\(@zone\\[[0-9]*\\]\\)=zone\$/\\1/p' | sed '1!G;h;\$!d'); do
		[ \"\$(uci -q get firewall.\$sect.name)\" = 'wan' ] && uci delete firewall.\$sect
	done

	brname=\$(uci -q get network.\$br.name)

	rm -rf /tmp/roamd-netrollback
	mkdir -p /tmp/roamd-netrollback
	cp /etc/config/network /etc/config/dhcp /etc/config/firewall /tmp/roamd-netrollback/

	uci commit network
	uci commit dhcp
	uci commit firewall

	/etc/init.d/dnsmasq disable
	/etc/init.d/odhcpd disable

	ubus call service add \"{\\\"name\\\":\\\"roamd-netapply\\\",\\\"instances\\\":{\\\"main\\\":{\\\"command\\\":[\\\"/bin/sh\\\",\\\"/usr/libexec/roamd/net-apply.sh\\\",\\\"\$brname\\\"]}}}\"

	echo \"\$(date '+%H:%M:%S') network: uplink=\$uplink bridge=\$br(\$brname) mac=$mac, restart scheduled\" >> /tmp/roamd-diag.log
" || fail network "cannot switch the node to the controller subnet"

sleep 20
newaddr=$(node_wait_by_mac "$mac" 180)
[ -n "$newaddr" ] || fail network "the node did not reappear in the controller subnet"
ADDR="$newaddr"
caddr=$(controller_addr_for "$ADDR")

node_ssh_long "$ADDR" "
	date -u -s '$(date -u '+%Y-%m-%d %H:%M:%S')' >/dev/null 2>&1
	/etc/init.d/sysntpd restart >/dev/null 2>&1
	uci set roamd.mesh.controller_addr='$caddr'
	uci commit roamd
	/etc/init.d/roamd restart >/dev/null 2>&1
" || fail network "the node is unreachable at $ADDR"

state=$(node_ssh "$ADDR" '
	printf "dhcp-server=%s lan=%s nat=%s" \
		"$(uci -q get dhcp.lan.ignore)" \
		"$(uci -q get network.lan.proto)" \
		"$(nft list ruleset 2>/dev/null | grep -c masquerade)"
' 2>/dev/null)

case "$state" in
	"dhcp-server=1 lan=dhcp nat=0") report network ok "$ADDR" ;;
	*) report network error "the node is still a router ($state) — single subnet not applied"; exit 1 ;;
esac

report profile progress "checking 802.11v support on the node"
wpad_out=$(node_ssh_long "$ADDR" '/usr/libexec/roamd/deps-ensure.sh' 2>&1)
wpad_rc=$?
wpad_out=$(echo "$wpad_out" | grep '^wpad:' | tail -1)
[ -n "$wpad_out" ] && report profile progress "$wpad_out"
[ "$wpad_rc" = "0" ] ||
	fail profile "the node has no 802.11v-capable wpad and it cannot be installed"

report enroll progress "securing the control channel"
mkdir -p /etc/roamd/members /etc/roamd/tokens
token=$(dd if=/dev/urandom bs=16 count=1 2>/dev/null | md5sum | cut -c1-24)

node_ssh_long "$ADDR" "
	if ! command -v openssl >/dev/null 2>&1; then
		if command -v apk >/dev/null 2>&1; then
			apk update >/dev/null 2>&1
			apk add openssl-util >/dev/null 2>&1
		else
			opkg update >/dev/null 2>&1
			opkg install openssl-util >/dev/null 2>&1
		fi
	fi
	command -v openssl >/dev/null 2>&1 || exit 3
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
		-keyout /etc/roamd-node.key -out /etc/roamd-node.crt -days 3650 \
		-subj '/CN=$ADDR' >/dev/null 2>&1
	uci set uhttpd.main.cert='/etc/roamd-node.crt'
	uci set uhttpd.main.key='/etc/roamd-node.key'
	uci commit uhttpd
	h=\$(openssl passwd -6 '$token')
	uci -q delete rpcd.roamd_mesh_login
	uci set rpcd.roamd_mesh_login=login
	uci set rpcd.roamd_mesh_login.username='mesh-$member_id'
	uci set rpcd.roamd_mesh_login.password=\"\$h\"
	uci add_list rpcd.roamd_mesh_login.read='roamd'
	uci add_list rpcd.roamd_mesh_login.write='roamd'
	uci commit rpcd
	/etc/init.d/rpcd reload >/dev/null 2>&1
	/etc/init.d/uhttpd restart >/dev/null 2>&1
"

case $? in
	0) ;;
	3) fail enroll "openssl is not available on the node, the control channel cannot be secured" ;;
	*) fail enroll "could not secure the control channel on the node" ;;
esac

node_scp_from "$ADDR" /etc/roamd-node.crt "/etc/roamd/members/${member_id}.crt" ||
	fail enroll "could not fetch the node certificate"

[ -s "/etc/roamd/members/${member_id}.crt" ] ||
	fail enroll "the node returned an empty certificate"

printf '%s' "$token" > "/etc/roamd/tokens/${member_id}"
chmod 600 "/etc/roamd/tokens/${member_id}"

report enroll progress "installing service key"
pubkey=$(ssh_pubkey)
if [ -n "$pubkey" ]; then
	node_ssh "$ADDR" "mkdir -p /etc/dropbear; grep -qF '$pubkey' /etc/dropbear/authorized_keys 2>/dev/null || printf '%s\n' '$pubkey' >> /etc/dropbear/authorized_keys; chmod 600 /etc/dropbear/authorized_keys"

	if node_ssh "$ADDR" "grep -qF '$pubkey' /etc/dropbear/authorized_keys"; then
		node_ssh "$ADDR" "
			rm -f /tmp/roamd-key-ok
			uci -q set dropbear.@dropbear[0].RootPasswordAuth='off'
			uci -q set dropbear.@dropbear[0].PasswordAuth='off'
			uci -q commit dropbear
			start-stop-daemon -S -b -x /bin/sh -- -c '
				sleep 90
				[ -f /tmp/roamd-key-ok ] && exit 0
				uci -q delete dropbear.@dropbear[0].RootPasswordAuth
				uci -q delete dropbear.@dropbear[0].PasswordAuth
				uci -q commit dropbear
				/etc/init.d/dropbear restart
			' >/dev/null 2>&1
			start-stop-daemon -S -b -x /bin/sh -- -c 'sleep 1; /etc/init.d/dropbear restart' >/dev/null 2>&1
		"
		sleep 12

		if node_ssh "$ADDR" 'touch /tmp/roamd-key-ok' >/dev/null 2>&1; then
			report enroll progress "service key works, password login disabled on the node"
		else
			report enroll progress "key login does not work, the node restores password login by itself in 90s"
		fi
	else
		report enroll progress "service key was not installed, password login left enabled on the node"
	fi
fi

ensure_curl || fail install "curl cannot be installed — no control channel to the node"

register_member "$member_id" "$mac" "$ADDR" "${hostname:-OpenWrt}"
ubus -t 3 call roamd reload >/dev/null 2>&1

report profile progress "waiting for the node to broadcast the network"
waited=0
while [ "$waited" -lt 90 ]; do
	aps=$(node_ssh "$ADDR" 'ubus list | grep -c "^hostapd\.phy"' 2>/dev/null | tr -d '\r\n ')
	[ -n "$aps" ] && [ "$aps" != "0" ] && break
	sleep 10
	waited=$((waited + 10))
done

if [ -n "$aps" ] && [ "$aps" != "0" ]; then
	report profile ok "the node broadcasts the network on $aps radio(s)"
else
	report profile error "the node has no access points up, check its wireless configuration"
fi

report done ok "$member_id"
