#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

OUT="/var/run/roamd/selfcheck.json"
mkdir -p /var/run/roamd

finish() {
	json_dump > "$OUT.tmp"
	mv "$OUT.tmp" "$OUT"
	exit 0
}

json_init

if ! versions=$(self_versions); then
	json_add_string error "index_unreachable"
	finish
fi

json_add_array packages
update=0

while read -r name have want; do
	[ -n "$name" ] || continue

	outdated=0
	sys_newer "$want" "$have" && outdated=1 && update=1

	json_add_object
	json_add_string name "$name"
	json_add_string installed "$have"
	json_add_string available "$want"
	json_add_boolean outdated "$outdated"
	json_close_object
done <<EOF
$versions
EOF

json_close_array
json_add_boolean update_available "$update"
finish
