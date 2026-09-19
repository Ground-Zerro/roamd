cmake_minimum_required(VERSION 3.10)

project(roamd C)

add_definitions(-Os -Wall -Werror -std=gnu99)

set(SOURCES
	main.c
	config.c
	provision.c
	bss.c
	sta.c
	policy.c
	pair.c
	mesh.c
	mesh_ctrl.c
	mesh_networks.c
	mesh_segment.c
	mesh_rpc.c
	mesh_pkg.c
	mesh_poll.c
	mesh_apply.c
	mesh_update.c
	mesh_node_pkg.c
	mesh_neigh.c
	mesh_discover.c
	mesh_member.c
	mesh_acquire.c
	mesh_deps.c
	mesh_vlan.c
	mesh_profile.c
	mesh_node.c
	mesh_bridge.c
	mesh_clients.c
	mesh_log.c
	ubus_api.c
	apk.c
	sha256.c
	mesh_remote.c
)

add_executable(roamd ${SOURCES})
target_link_libraries(roamd ubox ubus uci blobmsg_json uclient)

install(TARGETS roamd RUNTIME DESTINATION sbin)
