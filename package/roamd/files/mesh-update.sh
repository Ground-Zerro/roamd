#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

ADDR="$1"
TASK="$2"

task_open "$TASK"


report update progress "updating roamd on the node"

node_update "$ADDR" "$(member_id_by_addr "$ADDR")"
rc=$?

case $rc in
	0) ;;
	4) report update progress "repository unreachable, installed from the controller cache" ;;
	*) report "$(node_update_step $rc)" error "$(node_update_error $rc)"; exit 1 ;;
esac

report done ok updated
