#!/bin/sh
. /usr/libexec/roamd/mesh-lib.sh

STATE_FILE="/tmp/roamd/deps"
TRIES=3

say() {
	logger -t roamd "deps: $1"
	echo "deps: $1"
	diag_log "deps: $1"
}

finish() {
	mkdir -p /tmp/roamd
	printf '%s\n' "$2" > "$STATE_FILE"
	say "$2"
	[ "$1" = ok ] && exit 0
	exit 1
}

have_network() {
	local host

	for host in downloads.openwrt.org openwrt.org; do
		wget -q -s -T 4 "http://$host/" >/dev/null 2>&1 && return 0
	done

	return 1
}

installed_names() {
	local try out

	for try in $(seq $TRIES); do
		case "$mgr" in
			apk) out=$(apk info 2>/dev/null) ;;
			opkg) out=$(opkg list-installed 2>/dev/null | awk '{ print $1 }') ;;
		esac

		[ -n "$out" ] && { printf '%s\n' "$out"; return 0; }
		sleep 3
	done

	return 1
}

index_update() {
	[ -n "$index_done" ] && return 0

	sys_retry $TRIES 5 sys_index_update || return 1
	index_done=1
}

pkg_install() {
	sys_retry $TRIES 5 sys_add "$1"
}

current_pkg() {
	printf '%s\n' "$1" |
		grep -xE '(wpad|hostapd)(-(mini|basic|mesh))?(-(openssl|mbedtls|wolfssl))?' | head -1
}

full_variant() {
	case "$1" in
		*-openssl) echo wpad-openssl ;;
		*-mbedtls) echo wpad-mbedtls ;;
		*-wolfssl) echo wpad-wolfssl ;;
		*) echo wpad ;;
	esac
}

is_full() {
	case "$1" in
		wpad|wpad-openssl|wpad-mbedtls|wpad-wolfssl) return 0 ;;
	esac

	return 1
}

pkg_available() {
	local name="$1"

	case "$mgr" in
		apk) ( cd /tmp && apk fetch "$name" >/dev/null 2>&1 ) &&
			rm -f "/tmp/${name}"-[0-9]*.apk && return 0 ;;
		opkg) ( cd /tmp && opkg download "$name" >/dev/null 2>&1 ) &&
			rm -f "/tmp/${name}"_*.ipk && return 0 ;;
	esac

	return 1
}

replace_wpad() {
	local cur="$1" target="$2" back

	pkg_available "$target" || return 1

	case "$mgr" in
		apk)
			apk del "$cur" >/dev/null 2>&1
			pkg_install "$target" && return 0
			apk add "$cur" >/dev/null 2>&1
			;;
		opkg)
			back=$(ls -1 "/tmp/${cur}"_*.ipk 2>/dev/null | head -1)
			opkg remove "$cur" --force-depends >/dev/null 2>&1
			pkg_install "$target" && return 0
			[ -n "$back" ] && opkg install "$back" >/dev/null 2>&1
			;;
	esac

	return 1
}

wpad_reload() {
	[ -x /etc/init.d/wpad ] && /etc/init.d/wpad restart >/dev/null 2>&1
	sleep 3
	wifi down >/dev/null 2>&1
	sleep 2
	wifi up >/dev/null 2>&1
}

ensure_wpad() {
	local cur target

	cur=$(current_pkg "$installed")
	[ -n "$cur" ] || { problem="no hostapd package installed"; return 1; }

	is_full "$cur" && return 0

	target=$(full_variant "$cur")
	index_update || { problem="package index is not available, $cur kept — 802.11v unavailable"; return 1; }

	replace_wpad "$cur" "$target" || {
		problem="cannot replace $cur with $target — 802.11v unavailable"
		return 1
	}

	installed=$(installed_names) || { problem="$mgr gave no package list"; return 1; }
	is_full "$(current_pkg "$installed")" || {
		problem="$target did not take over from $cur — 802.11v unavailable"
		return 1
	}

	wpad_reload
	say "$cur replaced with $target for 802.11v"

	return 0
}

curl_needed() {
	[ "$(uci -q get roamd.mesh.role)" = "node" ] && return 1
	[ -n "$(member_sections)" ] || return 1
	command -v curl >/dev/null 2>&1 && return 1

	return 0
}

mgr=$(sys_mgr) || finish fail "no package manager found"

installed=$(installed_names) || finish fail "$mgr gave no package list"

problem=""
index_done=""

if ! is_full "$(current_pkg "$installed")" || curl_needed; then
	have_network || finish fail "no network yet, dependencies will be checked again later"
fi

ensure_wpad || finish fail "$problem"

if curl_needed && ! ensure_curl; then
	finish fail "curl cannot be installed — no control channel to the nodes"
fi

finish ok "$(current_pkg "$installed") supports 802.11v"
