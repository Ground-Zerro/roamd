#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <crypt.h>

#include "roamd.h"
#include "mesh.h"

struct ubus_context *ubus_ctx;

#define CRYPT_SALT_LEN	16

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

static int crypt_print(void)
{
	static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	char pass[128], setting[sizeof("$6$$") + CRYPT_SALT_LEN];
	unsigned char rnd[CRYPT_SALT_LEN];
	const char *hash;
	size_t i, got;
	FILE *f;

	if (!fgets(pass, sizeof(pass), stdin))
		return 1;

	pass[strcspn(pass, "\r\n")] = '\0';

	f = fopen("/dev/urandom", "r");
	if (!f)
		return 1;

	got = fread(rnd, 1, sizeof(rnd), f);
	fclose(f);
	if (got != sizeof(rnd))
		return 1;

	memcpy(setting, "$6$", 3);
	for (i = 0; i < CRYPT_SALT_LEN; i++)
		setting[3 + i] = alphabet[rnd[i] & 63];
	memcpy(setting + 3 + CRYPT_SALT_LEN, "$", 2);

	hash = crypt(pass, setting);
	if (!hash || hash[0] != '$')
		return 1;

	puts(hash);
	return 0;
}

static void ubus_connect_handler(struct ubus_context *ctx)
{
	ubus_ctx = ctx;
	uloop_timeout_set(&setup_timer, 1);
}

int main(int argc, char **argv)
{
	roam_time_update();
	roam_bss_setup();
	roam_sta_setup();
	roam_device_setup();

	if (argc == 3 && !strcmp(argv[1], "apk-index"))
		return apk_index_print(argv[2]) ? 0 : 1;

	if (argc == 3 && !strcmp(argv[1], "apk-block"))
		return apk_block_print(argv[2]) ? 0 : 1;

	if (argc == 2 && !strcmp(argv[1], "crypt"))
		return crypt_print();

	if (argc == 4 && !strcmp(argv[1], "net-apply"))
		return mesh_net_apply(argv[2], argv[3]);

	if (argc >= 2 && !strcmp(argv[1], "deps-ensure")) {
		roam_config_load();

		return mesh_deps_ensure(argc == 3 && !strcmp(argv[2], "need"));
	}

	if (argc == 4 && (!strcmp(argv[1], "acquire") || !strcmp(argv[1], "release"))) {
		int rc;

		roam_config_load();
		uloop_init();

		if (!strcmp(argv[1], "acquire"))
			rc = mesh_acquire_run(argv[2], argv[3]);
		else
			rc = mesh_release_run(argv[2], argv[3]);

		uloop_done();

		return rc;
	}

	if (argc == 2 && !strcmp(argv[1], "discover")) {
		int rc;

		roam_config_load();
		uloop_init();
		rc = mesh_discover_run();
		uloop_done();

		return rc;
	}

	if (argc >= 3 && (!strcmp(argv[1], "self-update") || !strcmp(argv[1], "node-update") ||
			  !strcmp(argv[1], "autoupdate"))) {
		int rc;

		roam_config_load();
		uloop_init();

		if (!strcmp(argv[1], "self-update"))
			rc = mesh_self_update(argv[2]);
		else if (!strcmp(argv[1], "node-update"))
			rc = argc == 4 ? mesh_node_update_task(argv[2], argv[3]) : 1;
		else
			rc = mesh_autoupdate_task(argc == 4 ? argv[2] : NULL,
						  argc == 4 ? argv[3] : argv[2]);

		uloop_done();

		return rc;
	}

	openlog("roamd", LOG_PID, LOG_DAEMON);

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
