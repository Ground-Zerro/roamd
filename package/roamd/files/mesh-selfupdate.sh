#!/bin/sh
. /usr/libexec/roamd/mesh-lib.sh

TASK="$2"

task_open "$TASK"


report update progress "installing packages on the controller"

outdated=$(self_outdated_packages) || fail update "package index is not available"
self_upgrade $outdated || fail update "package installation failed"

report done ok updated

( sleep 2; /etc/init.d/roamd restart ) >/dev/null 2>&1 &

exit 0
