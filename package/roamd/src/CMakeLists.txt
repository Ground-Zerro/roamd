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
	mesh_node.c
	mesh_bridge.c
	mesh_clients.c
	mesh_log.c
	ubus_api.c
	apk.c
)

add_executable(roamd ${SOURCES})
target_link_libraries(roamd ubox ubus uci blobmsg_json)

install(TARGETS roamd RUNTIME DESTINATION sbin)
