#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

. /etc/openwrt_release 2>/dev/null
branch="${DISTRIB_RELEASE%.*}"
arch="$DISTRIB_ARCH"

installed() {
	sed -n "/^Package: $1\$/,/^\$/s/^Version: //p" /usr/lib/opkg/status 2>/dev/null | head -1
	sed -n "/^P:$1\$/,/^\$/s/^V://p" /lib/apk/db/installed 2>/dev/null | head -1
}

json_init
json_add_string branch "$branch"
json_add_string arch "$arch"

if ! pkg_url >/dev/null; then
	json_add_string error "no_repository"
	json_dump
	exit 0
fi

json_add_array packages
update=0

for name in $PKG_MAIN $PKG_UI; do
	have=$(installed "$name" | head -1)
	[ -n "$have" ] || continue

	want=$(pkg_version "$branch" "$arch" "$name" 2>/dev/null)

	json_add_object
	json_add_string name "$name"
	json_add_string installed "$have"
	json_add_string available "${want:-}"
	json_add_boolean outdated "$([ -n "$want" ] && [ "$want" != "$have" ] && echo 1 || echo 0)"
	json_close_object

	[ -n "$want" ] && [ "$want" != "$have" ] && update=1
done

json_close_array
json_add_boolean update_available "$update"
json_dump
