RPC_NULL="00000000000000000000000000000000"

rpc_cacert() {
	local id="$1"
	[ -n "$id" ] && [ -f "/etc/roamd/members/${id}.crt" ] || return 1
	echo "--cacert /etc/roamd/members/${id}.crt"
}

rpc_token() {
	local id="$1"
	[ -n "$id" ] && [ -f "/etc/roamd/tokens/${id}" ] && cat "/etc/roamd/tokens/${id}"
}

rpc_login() {
	local addr="$1" id="$2"
	local user=root pass cacert
	pass=$(rpc_token "$id")
	[ -n "$pass" ] && user="mesh-${id}"

	cacert=$(rpc_cacert "$id") || return 1

	curl -s --max-time 8 $cacert "https://${addr}/ubus" \
		-d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"call\",\"params\":[\"$RPC_NULL\",\"session\",\"login\",{\"username\":\"$user\",\"password\":\"$pass\"}]}" \
		2>/dev/null | jsonfilter -e '@.result[1].ubus_rpc_session' 2>/dev/null
}

rpc_call() {
	local addr="$1" session="$2" object="$3" method="$4" args="$5" id="$6"
	local cacert
	[ -n "$args" ] || args='{}'
	cacert=$(rpc_cacert "$id") || return 1
	curl -s --max-time 12 $cacert "https://${addr}/ubus" \
		-d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"call\",\"params\":[\"$session\",\"$object\",\"$method\",$args]}" \
		2>/dev/null
}
