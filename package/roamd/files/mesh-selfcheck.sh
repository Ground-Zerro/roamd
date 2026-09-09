#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

feed_present() {
	case "$(sys_mgr)" in
		apk) grep -rqs roamd /etc/apk/repositories /etc/apk/repositories.d ;;
		opkg) grep -qs roamd /etc/opkg/customfeeds.conf ;;
		*) return 1 ;;
	esac
}

json_init

if ! feed_present; then
	json_add_string error "no_repository"
	json_dump
	exit 0
fi

if ! sys_index_update; then
	json_add_string error "index_unreachable"
	json_dump
	exit 0
fi

json_add_array packages
update=0

for name in $PKG_MAIN $PKG_UI; do
	have=$(sys_installed "$name") || continue
	want=$(sys_available "$name") || want="$have"

	json_add_object
	json_add_string name "$name"
	json_add_string installed "$have"
	json_add_string available "$want"
	json_add_boolean outdated "$([ "$want" != "$have" ] && echo 1 || echo 0)"
	json_close_object

	[ "$want" != "$have" ] && update=1
done

json_close_array
json_add_boolean update_available "$update"
json_dump
