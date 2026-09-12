#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "roamd.h"
#include "mesh.h"

struct ubus_context *ubus_ctx;

static struct ubus_auto_conn conn;
static struct uloop_timeout setup_timer;
static bool ubus_object_registered;

static void setup_run(struct uloop_timeout *t)
{
	if (!ubus_object_registered) {
		roam_ubus_object_init();
		ubus_object_registered = true;
	}

	roam_bss_init();
	roam_wireless_apply();
	mesh_start();

	roam_log(ROAM_L_INFO, "roamd: connected to ubus");
}

static void ubus_connect_handler(struct ubus_context *ctx)
{
	ubus_ctx = ctx;
	uloop_timeout_set(&setup_timer, 1);
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "apk-index"))
		return apk_index_print(argv[2]) ? 0 : 1;

	if (argc == 3 && !strcmp(argv[1], "apk-block"))
		return apk_block_print(argv[2]) ? 0 : 1;

	openlog("roamd", LOG_PID, LOG_DAEMON);

	roam_time_update();
	roam_bss_setup();
	roam_sta_setup();
	roam_device_setup();
	roam_config_load();

	if (!config.enabled) {
		roam_log(ROAM_L_INFO, "roamd: disabled by configuration");
		closelog();
		return 0;
	}

	uloop_init();

	setup_timer.cb = setup_run;
	conn.cb = ubus_connect_handler;
	ubus_auto_connect(&conn);

	uloop_run();

	uloop_timeout_cancel(&setup_timer);
	mesh_log_flush();
	mesh_clients_save();
	roam_bss_free_all();
	ubus_auto_shutdown(&conn);
	uloop_done();
	closelog();

	return 0;
}
