#!/bin/sh
. /usr/share/libubox/jshn.sh
. /usr/libexec/roamd/mesh-lib.sh

PHASE="$1"
TASK="$2"


record() {
	uci -q set "roamd.mesh.auto_update_last=$(date +%s)"
	uci -q set "roamd.mesh.auto_update_result=$1"
	uci -q commit roamd
}

update_nodes() {
	local updated="$1" failed="" sect id addr rc

	for sect in $(member_sections); do
		id=$(uci -q get "roamd.$sect.id")
		addr=$(uci -q get "roamd.$sect.addr")
		[ -n "$id" ] && [ -n "$addr" ] || continue
		member_needs_update "$id" || continue

		report update progress "updating roamd on the node"
		node_update "$addr" "$id"
		rc=$?

		case $rc in
			0) updated=1 ;;
			4) updated=1; report update progress "repository unreachable, installed from the controller cache" ;;
			*) failed=1; report "$(node_update_step $rc)" error "$(node_update_error $rc)" ;;
		esac
	done

	if [ -n "$updated" ]; then
		record updated
		report done ok "update installed"
	elif [ -n "$failed" ]; then
		record error
		report done error "some nodes were not updated"
	else
		record none
		report done ok "no updates"
	fi
}

if [ "$PHASE" = "nodes" ]; then
	task_attach "$TASK"
	update_nodes 1
	( sleep 2; /etc/init.d/roamd restart ) >/dev/null 2>&1 &
	exit 0
fi

task_open "$TASK"
report check progress "checking the repository"

if ! sys_retry 3 5 sys_index_update; then
	record error
	report check error "package index is not available"
	exit 1
fi

if self_outdated; then
	report update progress "installing packages on the controller"

	if ! self_upgrade; then
		record error
		report update error "package installation failed"
		exit 1
	fi

	report update progress "the controller is updated"
	exec /bin/sh "$0" nodes "$TASK"
fi

update_nodes ""
