DIAG_LOG="/tmp/roamd-diag.log"
DIAG_LOG_LINES=200

diag_log() {
	local n

	printf '%s %s\n' "$(date '+%H:%M:%S')" "$1" >> "$DIAG_LOG"

	n=$(wc -l < "$DIAG_LOG" 2>/dev/null) || return 0
	[ "$n" -gt $((DIAG_LOG_LINES * 2)) ] || return 0

	tail -n "$DIAG_LOG_LINES" "$DIAG_LOG" > "$DIAG_LOG.tmp" 2>/dev/null &&
		mv "$DIAG_LOG.tmp" "$DIAG_LOG"
}

ACQUIRE_DIR="/var/run/roamd/acquire"
ACQUIRE_KEEP=5

task_attach() {
	TASK_STATE="$ACQUIRE_DIR/$1"
}

task_open() {
	task_attach "$1"
	acquire_dir_trim
	: > "$TASK_STATE"
}

report() {
	printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$TASK_STATE"
}

fail() {
	report "$1" error "$2"
	exit 1
}

acquire_dir_trim() {
	mkdir -p "$ACQUIRE_DIR"
	ls -t "$ACQUIRE_DIR" 2>/dev/null | grep -v '\.mac$' | tail -n +$((ACQUIRE_KEEP + 1)) | while read -r old; do
		rm -f "$ACQUIRE_DIR/$old" "$ACQUIRE_DIR/$old.mac"
	done
}

SSH_OPTS="-o StrictHostKeyChecking=no -y -y"
SCP_OPTS="-o StrictHostKeyChecking=no"

node_forget() {
	[ -f /root/.ssh/known_hosts ] || return 0
	sed -i "/^$1 /d;/^\[$1\]/d" /root/.ssh/known_hosts 2>/dev/null
}
SSH_USER="root"
SSH_KEY="/etc/roamd/id"

ssh_key_opt() {
	[ -f "$SSH_KEY" ] && echo "-i $SSH_KEY"
}

ensure_ssh_key() {
	[ -f "$SSH_KEY" ] && return 0
	mkdir -p /etc/roamd
	dropbearkey -t rsa -s 2048 -f "$SSH_KEY" >/dev/null 2>&1
	chmod 600 "$SSH_KEY"
}

ssh_pubkey() {
	dropbearkey -y -f "$SSH_KEY" 2>/dev/null | grep '^ssh-'
}

run_bounded() {
	local limit="$1"; shift
	"$@" &
	local pid=$!
	local waited=0

	while kill -0 "$pid" 2>/dev/null; do
		[ "$waited" -ge "$limit" ] && {
			kill "$pid" 2>/dev/null
			sleep 2
			kill -9 "$pid" 2>/dev/null
			wait "$pid" 2>/dev/null
			return 124
		}
		sleep 1
		waited=$((waited + 1))
	done

	wait "$pid"
}

node_ssh() {
	local addr="$1"; shift
	run_bounded 8 ssh $SSH_OPTS $(ssh_key_opt) "${SSH_USER}@${addr}" "$@"
}

node_ssh_long() {
	local addr="$1"; shift
	run_bounded 300 ssh $SSH_OPTS $(ssh_key_opt) "${SSH_USER}@${addr}" "$@"
}

addr_is_v6() {
	case "$1" in *:*) return 0;; esac

	return 1
}

scp_host() {
	addr_is_v6 "$1" && { echo "[$1]"; return 0; }
	echo "$1"
}

node_scp() {
	local src="$1" addr="$2" dst="$3"
	run_bounded 60 scp $SCP_OPTS $(ssh_key_opt) "$src" "${SSH_USER}@$(scp_host "$addr"):${dst}"
}

node_scp_from() {
	local addr="$1" src="$2" dst="$3"

	run_bounded 60 ssh $SSH_OPTS $(ssh_key_opt) "${SSH_USER}@${addr}" "cat '$src'" > "$dst" 2>/dev/null || {
		rm -f "$dst"
		return 1
	}

	[ -s "$dst" ] || {
		rm -f "$dst"
		return 1
	}
}

PKG_DIR="/usr/libexec/roamd/pkg"

node_arch() {
	node_ssh "$1" '. /etc/openwrt_release 2>/dev/null; echo "$DISTRIB_ARCH"' 2>/dev/null | tr -d '\r\n '
}

node_pkgmgr() {
	node_ssh "$1" 'command -v apk >/dev/null && echo apk || { command -v opkg >/dev/null && echo opkg; }' 2>/dev/null | tr -d '\r\n '
}

PKG_MAIN="roamd"
PKG_UI="luci-app-roamd luci-i18n-roamd-ru"

SYS_MGR=""

sys_mgr() {
	if [ -z "$SYS_MGR" ]; then
		command -v apk >/dev/null 2>&1 && SYS_MGR=apk
		[ -n "$SYS_MGR" ] || { command -v opkg >/dev/null 2>&1 && SYS_MGR=opkg; }
	fi

	[ -n "$SYS_MGR" ] || return 1

	echo "$SYS_MGR"
}

sys_retry() {
	local tries="$1" delay="$2" try=1
	shift 2

	while :; do
		"$@" && return 0
		[ "$try" -lt "$tries" ] || return 1
		try=$((try + 1))
		sleep "$delay"
	done
}

sys_index_update() {
	case "$(sys_mgr)" in
		apk) apk update >/dev/null 2>&1 ;;
		opkg) opkg update >/dev/null 2>&1 ;;
		*) return 1 ;;
	esac
}

sys_add() {
	case "$(sys_mgr)" in
		apk) apk add "$@" >/dev/null 2>&1 ;;
		opkg) opkg install "$@" >/dev/null 2>&1 ;;
		*) return 1 ;;
	esac
}

sys_upgrade() {
	[ "$#" -gt 0 ] || return 1

	case "$(sys_mgr)" in
		apk) apk upgrade "$@" >/dev/null 2>&1 ;;
		opkg) opkg upgrade "$@" >/dev/null 2>&1 ;;
		*) return 1 ;;
	esac
}

sys_newer() {
	case "$(sys_mgr)" in
		apk) [ "$(apk version -t "$1" "$2" 2>/dev/null)" = ">" ] ;;
		opkg) opkg compare-versions "$1" ">>" "$2" ;;
		*) return 1 ;;
	esac
}

sys_installed() {
	local version

	case "$(sys_mgr)" in
		apk) version=$(sed -n "/^P:$1\$/,/^\$/s/^V://p" /lib/apk/db/installed 2>/dev/null | head -1) ;;
		opkg) version=$(opkg list-installed 2>/dev/null | awk -v n="$1" '$1 == n { print $3 }' | head -1) ;;
	esac

	[ -n "$version" ] || return 1

	echo "$version"
}

sys_available() {
	local version

	case "$(sys_mgr)" in
		apk) version=$(apk list -u "$1" 2>/dev/null | awk 'NR == 1 { print $1 }' | sed "s/^$1-//") ;;
		opkg) version=$(opkg list-upgradable 2>/dev/null | awk -v n="$1" '$1 == n { print $NF }' | head -1) ;;
	esac

	[ -n "$version" ] || return 1

	echo "$version"
}

pkg_local() {
	local name="${3:-$PKG_MAIN}"
	ls "$PKG_DIR/$1/$2/$name"-[0-9]*.apk "$PKG_DIR/$1/$2/${name}_"*.ipk 2>/dev/null | head -1
}

PKG_CERT="/etc/roamd/pkg.crt"

PKG_URL_EFFECTIVE=""

pkg_url() {
	if [ -z "$PKG_URL_EFFECTIVE" ]; then
		PKG_URL_EFFECTIVE=$(uci -q get roamd.mesh.pkg_url)
		[ -n "$PKG_URL_EFFECTIVE" ] ||
			PKG_URL_EFFECTIVE=$(ubus -t 3 call roamd mesh_status 2>/dev/null |
				jsonfilter -e '@.pkg_url')
	fi

	[ -n "$PKG_URL_EFFECTIVE" ] || return 1

	case "$PKG_URL_EFFECTIVE" in
		https://*) ;;
		*) return 3 ;;
	esac

	echo "$PKG_URL_EFFECTIVE"
}

feed_url() {
	local url branch="$1" arch="$2"

	url=$(pkg_url) || return $?

	case "$url" in
		*%b*|*%a*) echo "$url" | sed "s|%b|$branch|g; s|%a|$arch|g" ;;
		*) echo "$url/$branch/$arch" ;;
	esac
}

pkg_get() {
	if [ -f "$PKG_CERT" ]; then
		curl -q -sfL --max-time 20 --cacert "$PKG_CERT" "$1" -o "${2:--}"
	else
		curl -q -sfL --max-time 20 "$1" -o "${2:--}"
	fi
}

pkg_index_meta() {
	local url index adb rc meta name="${3:-$PKG_MAIN}"

	url=$(feed_url "$1" "$2") || return 2

	index=$(pkg_get "$url/Packages")
	rc=$?

	if [ "$rc" = 0 ]; then
		meta=$(printf '%s\n' "$index" | awk -v want="Package: $name" '
			$0 == want { in_pkg = 1 }
			/^$/ { in_pkg = 0 }
			in_pkg && /^Version:/ { version = $2 }
			in_pkg && /^Filename:/ { name = $2 }
			in_pkg && /^SHA256sum:/ { sum = $2 }
			END { if (name != "" && sum != "") print name, sum, version, "file" }')
	else
		[ "$rc" = 22 ] || return 2

		adb=$(mktemp)
		pkg_get "$url/packages.adb" "$adb"
		rc=$?
		[ "$rc" = 0 ] && meta=$(roamd apk-index "$adb" |
			awk -v want="$name" '$1 == want { print want "-" $2 ".apk", $4, $2, "adb" }')
		rm -f "$adb"

		case $rc in
			0) ;;
			22) return 1 ;;
			*) return 2 ;;
		esac
	fi

	[ -n "$meta" ] || return 1

	echo "$meta"
}

pkg_digest() {
	case "$2" in
		adb) roamd apk-block "$1" 2>/dev/null | sha256sum ;;
		*) sha256sum "$1" 2>/dev/null ;;
	esac | cut -d' ' -f1
}

pkg_version() {
	local meta file name="${3:-$PKG_MAIN}"

	meta=$(pkg_index_meta "$1" "$2" "$name")
	if [ -n "$meta" ]; then
		echo "$meta" | cut -d' ' -f3
		return 0
	fi

	file=$(pkg_local "$1" "$2" "$name")
	[ -n "$file" ] || return 1

	case "$file" in
		*.apk) basename "$file" .apk | sed -n "s/^$name-\\(.*\\)$/\\1/p" ;;
		*) basename "$file" | sed -n "s/^${name}_\\([^_]*\\)_.*/\\1/p" ;;
	esac
}

pkg_fetch() {
	local branch="$1" arch="$2" meta name sum kind path

	meta=$(pkg_index_meta "$branch" "$arch" "${3:-$PKG_MAIN}")
	[ -n "$meta" ] || return 1

	name=${meta%% *}
	sum=$(echo "$meta" | cut -d' ' -f2)
	kind=${meta##* }

	path="$PKG_DIR/$branch/$arch/$name"
	mkdir -p "$PKG_DIR/$branch/$arch"

	if [ "$(pkg_digest "$path" "$kind")" != "$sum" ]; then
		pkg_get "$(feed_url "$branch" "$arch")/$name" "$path" || { rm -f "$path"; return 1; }
		[ "$(pkg_digest "$path" "$kind")" = "$sum" ] || { rm -f "$path"; return 1; }
	fi

	echo "$path"
}

node_has_luci() {
	node_ssh "$1" '[ -d /www/luci-static/resources ]' >/dev/null 2>&1
}

pkg_push() {
	local addr="$1" pkg="$2" mgr="$3" opt="$4"
	local file sum cmd rc waited=0

	file=$(basename "$pkg")
	sum=$(sha256sum "$pkg" | cut -d' ' -f1)

	case "$mgr" in
		apk) cmd="apk add --allow-untrusted --repositories-file /dev/null '/tmp/$file'" ;;
		opkg) cmd="opkg install $opt '/tmp/$file'" ;;
		*) return 1 ;;
	esac

	node_scp "$pkg" "$addr" "/tmp/$file" || return 1
	node_ssh "$addr" "
		echo '$sum  /tmp/$file' | sha256sum -c >/dev/null 2>&1 || { rm -f '/tmp/$file'; exit 1; }
		rm -f /tmp/roamd-pkg.rc
		cat > /tmp/roamd-pkg.sh <<'EOS'
#!/bin/sh
$cmd </dev/null >>/tmp/roamd-install.log 2>&1
echo \$? > /tmp/roamd-pkg.rc
rm -f '/tmp/$file'
ubus call service delete '{\"name\":\"roamd-pkg\"}'
EOS
		ubus call service add \"{\\\"name\\\":\\\"roamd-pkg\\\",\\\"instances\\\":{\\\"main\\\":{\\\"command\\\":[\\\"/bin/sh\\\",\\\"/tmp/roamd-pkg.sh\\\"]}}}\"
	" || return 1

	while [ "$waited" -lt 240 ]; do
		sleep 5
		waited=$((waited + 5))
		rc=$(node_ssh "$addr" 'cat /tmp/roamd-pkg.rc 2>/dev/null' | tr -d '\r\n ')
		[ -n "$rc" ] && {
			node_ssh "$addr" "rm -f '/tmp/$file' /tmp/roamd-pkg.sh /tmp/roamd-pkg.rc" >/dev/null 2>&1
			return "$rc"
		}
	done

	return 1
}

install_roamd_pkg() {
	local addr="$1" branch="$2" force="$3"
	local arch mgr names name pkg repo cached="" opt=""
	[ "$force" = "force" ] && opt="--force-reinstall"

	arch=$(node_arch "$addr")
	[ -n "$arch" ] || return 1

	pkg_url >/dev/null
	case $? in
		0) repo=1 ;;
		1) repo="" ;;
		*) return 3 ;;
	esac

	mgr=$(node_pkgmgr "$addr")
	node_ssh "$addr" ': > /tmp/roamd-install.log' >/dev/null 2>&1

	names="$PKG_MAIN"
	node_has_luci "$addr" && names="$names $PKG_UI"

	for name in $names; do
		pkg=""
		[ -n "$repo" ] && pkg=$(pkg_fetch "$branch" "$arch" "$name")

		if [ -z "$pkg" ]; then
			pkg=$(pkg_local "$branch" "$arch" "$name")
			[ -n "$pkg" ] && [ "$name" = "$PKG_MAIN" ] && cached=1
		fi

		if [ -n "$pkg" ]; then
			pkg_push "$addr" "$pkg" "$mgr" "$opt" && continue
			[ "$name" = "$PKG_MAIN" ] && return 1
			continue
		fi

		[ "$name" = "$PKG_MAIN" ] || continue

		case "$mgr" in
			apk) node_ssh_long "$addr" 'apk update >/dev/null 2>&1 && apk add roamd >/dev/null 2>&1' || return 2 ;;
			opkg) node_ssh_long "$addr" 'opkg update >/dev/null 2>&1 && opkg install roamd >/dev/null 2>&1' || return 2 ;;
			*) return 1 ;;
		esac
	done

	for try in 1 2 3 4 5; do
		node_ssh "$addr" 'command -v roamd >/dev/null' && break
		[ "$try" = 5 ] && return 1
		sleep 5
	done

	[ -n "$cached" ] && return 4
	return 0
}

MEMBERS_DIR="/var/run/roamd/members"

ensure_curl() {
	command -v curl >/dev/null 2>&1 && return 0

	sys_retry 2 5 sys_index_update || return 1
	sys_add curl >/dev/null 2>&1
	command -v curl >/dev/null 2>&1 || return 1

	logger -t roamd "mesh: curl installed for the control channel"
	diag_log "curl installed for the mesh control channel"

	return 0
}

AP_SECTION=""

ap_section_find() {
	local sect

	[ -z "$AP_SECTION" ] || return 0

	for sect in $(uci -q show wireless | sed -n 's/^wireless\.\([^.]*\)=wifi-iface$/\1/p'); do
		case "$sect" in mesh_bh_*) continue;; esac
		[ "$(uci -q get "wireless.$sect.mode")" = "ap" ] || continue
		AP_SECTION="$sect"
		return 0
	done

	return 1
}

ap_field() {
	[ -n "$AP_SECTION" ] || return 0
	uci -q get "wireless.$AP_SECTION.$1"
}

member_sections() {
	uci -q show roamd | sed -n 's/^roamd\.\(@member\[[0-9]*\]\|[a-z0-9_]*\)=member$/\1/p'
}

member_id_by_addr() {
	local sect

	for sect in $(member_sections); do
		[ "$(uci -q get "roamd.$sect.addr")" = "$1" ] || continue
		uci -q get "roamd.$sect.id"
		return
	done
}

member_pkg_source() {
	[ -n "$1" ] || return 0

	mkdir -p "$MEMBERS_DIR"
	printf '%s' "$2" > "$MEMBERS_DIR/$1.pkgsrc"
}

member_needs_update() {
	[ "$(jsonfilter -e '@.update_available' < "$MEMBERS_DIR/$1.json" 2>/dev/null)" = "true" ]
}

NODE_UPDATE_UNREACHABLE=5
NODE_UPDATE_NO_RELEASE=6

node_update() {
	local addr="$1" id="$2" board version branch rc

	node_forget "$addr"

	board=$(node_ssh "$addr" 'ubus call system board' 2>/dev/null)
	[ -n "$board" ] || return "$NODE_UPDATE_UNREACHABLE"
	json_load "$board" 2>/dev/null || return "$NODE_UPDATE_UNREACHABLE"
	json_select release 2>/dev/null && json_get_var version version

	branch="${version%.*}"
	[ -n "$branch" ] || return "$NODE_UPDATE_NO_RELEASE"

	install_roamd_pkg "$addr" "$branch" force
	rc=$?

	case $rc in
		0) member_pkg_source "$id" repository ;;
		4) member_pkg_source "$id" cache ;;
		*) return $rc ;;
	esac

	node_ssh "$addr" '/etc/init.d/roamd restart >/dev/null 2>&1'

	return $rc
}

node_update_step() {
	case "$1" in
		2|"$NODE_UPDATE_NO_RELEASE") echo compat ;;
		"$NODE_UPDATE_UNREACHABLE") echo probe ;;
		*) echo update ;;
	esac
}

node_update_error() {
	case "$1" in
		2) echo "no package for the OpenWrt version of the node" ;;
		3) echo "package source is not trusted: pkg_url must be https and /etc/roamd/pkg.crt must exist" ;;
		"$NODE_UPDATE_UNREACHABLE") echo "node is unreachable" ;;
		"$NODE_UPDATE_NO_RELEASE") echo "unknown OpenWrt version" ;;
		*) echo "package installation failed on the node" ;;
	esac
}

self_packages() {
	local name names=""

	for name in $PKG_MAIN $PKG_UI; do
		sys_installed "$name" >/dev/null && names="$names $name"
	done

	echo "$names"
}

self_outdated() {
	local name have want

	for name in $(self_packages); do
		have=$(sys_installed "$name") || continue
		want=$(sys_available "$name") || continue
		[ "$want" != "$have" ] && return 0
	done

	return 1
}

self_upgrade() {
	local names

	names=$(self_packages)
	[ -n "$names" ] || return 1

	# shellcheck disable=SC2086
	sys_retry 3 10 sys_upgrade $names
}

lan_device() {
	ubus -t 3 call network.interface.lan status 2>/dev/null | jsonfilter -e '@.l3_device'
}

lan_subnets() {
	ip -4 -o addr show dev "$(lan_device)" scope global 2>/dev/null | while read -r _ _ _ cidr _; do
		[ "${cidr#*/}" -ge 24 ] || continue
		case "$cidr" in
			10.*|192.168.*|172.1[6-9].*|172.2[0-9].*|172.3[01].*) echo "$cidr";;
		esac
	done | sort -u
}

subnet_base() { echo "${1%.*/*}"; }

warm_subnet() {
	local base="$1" i
	for i in $(seq 1 254); do
		ping -c1 -W1 "${base}.${i}" >/dev/null 2>&1 &
	done
	wait
}

node_addr_by_mac() {
	local mac="$1" sub

	for sub in $(lan_subnets); do
		warm_subnet "$(subnet_base "$sub")"
	done

	ip neigh show dev "$(lan_device)" | awk -v m="$mac" '$2 == "lladdr" && tolower($3) == tolower(m) { print $1; exit }'
}

node_wait_by_mac() {
	local mac="$1" limit="${2:-120}" waited=0 addr

	while [ "$waited" -lt "$limit" ]; do
		addr=$(node_addr_by_mac "$mac")
		if [ -n "$addr" ] && node_ssh "$addr" true >/dev/null 2>&1; then
			echo "$addr"
			return 0
		fi
		sleep 5
		waited=$((waited + 5))
	done

	return 1
}

own_addrs() {
	ip -4 -o addr show scope global | while read -r _ _ _ cidr _; do echo "${cidr%/*}"; done
}

ll_neighbours() {
	local dev="$1" self

	ping6 -c 2 -w 2 "ff02::1%$dev" >/dev/null 2>&1
	self=$(ip -6 -o addr show dev "$dev" scope link 2>/dev/null | awk '{ print $4 }' | cut -d/ -f1 | tr '\n' ' ')

	ip -6 neigh show dev "$dev" 2>/dev/null | awk -v self="$self" -v dev="$dev" '
		$1 ~ /^fe80:/ && $2 == "lladdr" {
			if (index(self, $1) == 0)
				print $1 "%" dev
		}' | sort -u
}
