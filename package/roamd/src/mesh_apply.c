#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "roamd.h"
#include "mesh.h"

#define APPLY_LOG	"/tmp/roamd-diag.log"
#define APPLY_BACKUP	"/tmp/roamd-netrollback"
#define APPLY_WAIT	120
#define APPLY_STEP	5

static void apply_log(const char *text)
{
	FILE *f = fopen(APPLY_LOG, "a");
	time_t now = time(NULL);
	struct tm tm;

	if (!f)
		return;

	localtime_r(&now, &tm);
	fprintf(f, "%02d:%02d:%02d net-apply: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, text);
	fclose(f);
}

static void service_ctl(const char *name, const char *action)
{
	char cmd[96];

	snprintf(cmd, sizeof(cmd), "/etc/init.d/%s %s >/dev/null 2>&1", name, action);
	if (system(cmd))
		return;
}

static void unregister(void)
{
	if (system("ubus call service delete '{\"name\":\"roamd-netapply\"}' >/dev/null 2>&1"))
		return;
}

static void rollback(void)
{
	static const char *const files[] = { "network", "dhcp", "firewall" };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		char cmd[160];

		snprintf(cmd, sizeof(cmd), "cp %s/%s /etc/config/%s 2>/dev/null",
			 APPLY_BACKUP, files[i], files[i]);
		if (system(cmd))
			continue;
	}

	service_ctl("dnsmasq", "enable");
	service_ctl("odhcpd", "enable");
	service_ctl("dnsmasq", "start");
	service_ctl("odhcpd", "start");
	service_ctl("firewall", "restart");
	service_ctl("network", "restart");
}

int mesh_net_apply(const char *bridge, const char *controller)
{
	char text[128];
	int waited = 0;

	if (!bridge || !controller)
		return 1;

	snprintf(text, sizeof(text), "starting on %s, controller %s", bridge, controller);
	apply_log(text);

	service_ctl("dnsmasq", "stop");
	service_ctl("odhcpd", "stop");
	unlink("/tmp/resolv.conf");
	if (symlink("/tmp/resolv.conf.d/resolv.conf.auto", "/tmp/resolv.conf"))
		apply_log("resolv.conf link kept as is");

	service_ctl("firewall", "restart");
	service_ctl("network", "restart");

	while (waited < APPLY_WAIT) {
		sleep(APPLY_STEP);
		waited += APPLY_STEP;

		if (!mesh_icmp_ping(controller, 2000))
			continue;

		snprintf(text, sizeof(text), "controller %s answers after %ds", controller, waited);
		apply_log(text);
		unregister();

		return 0;
	}

	snprintf(text, sizeof(text), "controller %s does not answer after %ds, rolling back",
		 controller, waited);
	apply_log(text);
	rollback();
	unregister();

	return 1;
}
