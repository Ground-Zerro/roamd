#!/bin/sh
. /usr/libexec/roamd/mesh-lib.sh

TASK="$2"
STATE="$ACQUIRE_DIR/$TASK"

acquire_dir_trim
report() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$STATE"; }
fail() { report update error "$1"; exit 1; }

: > "$STATE"
report update progress "installing packages on the controller"

sys_retry 3 5 sys_index_update || fail "package index is not available"

names=""
for name in $PKG_MAIN $PKG_UI; do
	sys_installed "$name" >/dev/null && names="$names $name"
done

# shellcheck disable=SC2086
sys_retry 3 10 sys_upgrade $names || fail "package installation failed"

report done ok updated

( sleep 2; /etc/init.d/roamd restart ) >/dev/null 2>&1 &

exit 0
