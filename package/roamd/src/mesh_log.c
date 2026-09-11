#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include <libubox/uloop.h>
#include <libubox/blobmsg_json.h>

#include "roamd.h"
#include "mesh.h"

#define MESH_LOG_MAX	200
#define MESH_LOG_DIR	"/tmp/roamd"
#define MESH_LOG_FILE	MESH_LOG_DIR "/mesh_log.jsonl"
#define MESH_LOG_FLUSH	300000

static struct mesh_event ring[MESH_LOG_MAX];
static unsigned int ring_count;
static unsigned int ring_head;
static uint32_t ring_seq;
static bool ring_dirty;
static struct uloop_timeout flush_timer;

static const char *const type_name[] = {
	[MESH_EV_CONNECT] = "connect",
	[MESH_EV_DISCONNECT] = "disconnect",
	[MESH_EV_ROAM] = "roam",
	[MESH_EV_STEER] = "steer",
	[MESH_EV_KICK] = "kick",
};

const char *mesh_event_type_name(enum mesh_event_type t)
{
	if (t < ARRAY_SIZE(type_name) && type_name[t])
		return type_name[t];
	return "";
}

static const char *band_str(uint8_t band)
{
	if (band == BAND_LOW)
		return "2.4";
	if (band == BAND_HIGH)
		return "5";
	return "";
}

static struct mesh_event *ring_insert(const struct mesh_event *ev)
{
	unsigned int idx = (ring_head + ring_count) % MESH_LOG_MAX;
	struct mesh_event *slot = &ring[idx];

	*slot = *ev;

	if (ring_count < MESH_LOG_MAX)
		ring_count++;
	else
		ring_head = (ring_head + 1) % MESH_LOG_MAX;

	return slot;
}

static void event_line(char *buf, size_t size, const struct mesh_event *ev)
{
	snprintf(buf, size,
		"{\"seq\":%u,\"ts\":%u,\"mac\":\"%s\",\"from_node\":\"%s\","
		"\"to_node\":\"%s\",\"from_band\":%u,\"to_band\":%u,\"type\":%u}\n",
		ev->seq, ev->ts, ev->mac, ev->from_node, ev->to_node,
		ev->from_band, ev->to_band, ev->type);
}

void mesh_log_flush(void)
{
	FILE *f;
	char line[512];
	unsigned int i;

	if (!ring_dirty)
		return;

	mkdir(MESH_LOG_DIR, 0755);
	f = fopen(MESH_LOG_FILE, "w");
	if (!f)
		return;

	for (i = 0; i < ring_count; i++) {
		event_line(line, sizeof(line), &ring[(ring_head + i) % MESH_LOG_MAX]);
		fputs(line, f);
	}

	fclose(f);
	ring_dirty = false;
}

static void flush_cb(struct uloop_timeout *t)
{
	mesh_log_flush();
	mesh_clients_save();
	uloop_timeout_set(&flush_timer, MESH_LOG_FLUSH);
}

void mesh_log_push(const struct mesh_event *ev)
{
	struct mesh_event *slot = ring_insert(ev);

	slot->seq = ++ring_seq;
	if (!slot->ts)
		slot->ts = time(NULL);

	ring_dirty = true;

	if (!flush_timer.cb) {
		flush_timer.cb = flush_cb;
		uloop_timeout_set(&flush_timer, MESH_LOG_FLUSH);
	}
}

enum { LF_SEQ, LF_TS, LF_MAC, LF_FN, LF_TN, LF_FB, LF_TB, LF_TYPE, __LF_MAX };

static const struct blobmsg_policy lf_policy[__LF_MAX] = {
	[LF_SEQ] = { .name = "seq", .type = BLOBMSG_TYPE_INT32 },
	[LF_TS] = { .name = "ts", .type = BLOBMSG_TYPE_INT32 },
	[LF_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[LF_FN] = { .name = "from_node", .type = BLOBMSG_TYPE_STRING },
	[LF_TN] = { .name = "to_node", .type = BLOBMSG_TYPE_STRING },
	[LF_FB] = { .name = "from_band", .type = BLOBMSG_TYPE_INT32 },
	[LF_TB] = { .name = "to_band", .type = BLOBMSG_TYPE_INT32 },
	[LF_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_INT32 },
};

void mesh_log_load(void)
{
	static struct blob_buf b;
	FILE *f = fopen(MESH_LOG_FILE, "r");
	char line[512];

	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		struct blob_attr *tb[__LF_MAX];
		struct mesh_event ev;

		blob_buf_init(&b, 0);
		if (!blobmsg_add_json_from_string(&b, line))
			continue;

		blobmsg_parse(lf_policy, __LF_MAX, tb, blob_data(b.head), blob_len(b.head));
		if (!tb[LF_MAC])
			continue;

		memset(&ev, 0, sizeof(ev));
		ev.seq = tb[LF_SEQ] ? blobmsg_get_u32(tb[LF_SEQ]) : 0;
		ev.ts = tb[LF_TS] ? blobmsg_get_u32(tb[LF_TS]) : 0;
		strncpy(ev.mac, blobmsg_get_string(tb[LF_MAC]), sizeof(ev.mac) - 1);
		if (tb[LF_FN])
			strncpy(ev.from_node, blobmsg_get_string(tb[LF_FN]), sizeof(ev.from_node) - 1);
		if (tb[LF_TN])
			strncpy(ev.to_node, blobmsg_get_string(tb[LF_TN]), sizeof(ev.to_node) - 1);
		ev.from_band = tb[LF_FB] ? blobmsg_get_u32(tb[LF_FB]) : 0xff;
		ev.to_band = tb[LF_TB] ? blobmsg_get_u32(tb[LF_TB]) : 0xff;
		ev.type = tb[LF_TYPE] ? blobmsg_get_u32(tb[LF_TYPE]) : 0;

		ring_insert(&ev);
		if (ev.seq > ring_seq)
			ring_seq = ev.seq;
	}

	fclose(f);

	ring_dirty = true;
	mesh_log_flush();
}

void mesh_log_local(const char *mac, uint8_t from_band, uint8_t to_band,
		    enum mesh_event_type type)
{
	struct mesh_event ev;
	const char *node = mesh.member_id[0] ? mesh.member_id : "";

	memset(&ev, 0, sizeof(ev));
	strncpy(ev.mac, mac, sizeof(ev.mac) - 1);
	strncpy(ev.from_node, node, sizeof(ev.from_node) - 1);
	strncpy(ev.to_node, node, sizeof(ev.to_node) - 1);
	ev.from_band = from_band;
	ev.to_band = to_band;
	ev.type = type;

	mesh_log_push(&ev);
}

static void event_to_blob(struct blob_buf *b, const struct mesh_event *ev)
{
	void *e = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "seq", ev->seq);
	blobmsg_add_u32(b, "ts", ev->ts);
	blobmsg_add_string(b, "mac", ev->mac);
	blobmsg_add_string(b, "from_node", ev->from_node);
	blobmsg_add_string(b, "to_node", ev->to_node);
	blobmsg_add_string(b, "from_band", band_str(ev->from_band));
	blobmsg_add_string(b, "to_band", band_str(ev->to_band));
	blobmsg_add_string(b, "type", mesh_event_type_name(ev->type));
	blobmsg_close_table(b, e);
}

void mesh_log_dump(struct blob_buf *b, unsigned int limit)
{
	void *arr = blobmsg_open_array(b, "events");
	unsigned int shown = 0, i;

	if (!limit || limit > ring_count)
		limit = ring_count;

	for (i = ring_count; i > 0 && shown < limit; i--, shown++) {
		unsigned int idx = (ring_head + i - 1) % MESH_LOG_MAX;

		event_to_blob(b, &ring[idx]);
	}

	blobmsg_close_array(b, arr);
}

enum {
	EVF_SEQ,
	EVF_TS,
	EVF_MAC,
	EVF_FROM_BAND,
	EVF_TO_BAND,
	EVF_TYPE,
	__EVF_MAX
};

static const struct blobmsg_policy ev_policy[__EVF_MAX] = {
	[EVF_SEQ] = { .name = "seq", .type = BLOBMSG_TYPE_INT32 },
	[EVF_TS] = { .name = "ts", .type = BLOBMSG_TYPE_INT32 },
	[EVF_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[EVF_FROM_BAND] = { .name = "from_band", .type = BLOBMSG_TYPE_STRING },
	[EVF_TO_BAND] = { .name = "to_band", .type = BLOBMSG_TYPE_STRING },
	[EVF_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_STRING },
};

void mesh_log_ingest(const char *node, struct blob_attr *events)
{
	struct mesh_member *m = mesh_member_by_name(node);
	uint32_t last = m ? m->log_seq : 0;
	uint32_t maxseq = last;
	struct blob_attr *cur;
	int rem;

	blobmsg_for_each_attr(cur, events, rem) {
		struct blob_attr *tb[__EVF_MAX];
		uint32_t seq;

		blobmsg_parse(ev_policy, __EVF_MAX, tb, blobmsg_data(cur), blobmsg_data_len(cur));
		if (!tb[EVF_SEQ] || !tb[EVF_MAC])
			continue;

		seq = blobmsg_get_u32(tb[EVF_SEQ]);
		if (seq <= last)
			continue;

		mesh_log_merge(node, blobmsg_get_string(tb[EVF_MAC]),
			       tb[EVF_TS] ? blobmsg_get_u32(tb[EVF_TS]) : 0,
			       tb[EVF_FROM_BAND] ? blobmsg_get_string(tb[EVF_FROM_BAND]) : "",
			       tb[EVF_TO_BAND] ? blobmsg_get_string(tb[EVF_TO_BAND]) : "",
			       tb[EVF_TYPE] ? blobmsg_get_string(tb[EVF_TYPE]) : "roam");

		if (seq > maxseq)
			maxseq = seq;
	}

	if (m)
		m->log_seq = maxseq;
}

void mesh_log_merge(const char *node, const char *mac, uint32_t ts,
		    const char *from_band, const char *to_band, const char *type)
{
	struct mesh_event ev;
	size_t i;

	memset(&ev, 0, sizeof(ev));
	strncpy(ev.mac, mac, sizeof(ev.mac) - 1);
	strncpy(ev.from_node, node, sizeof(ev.from_node) - 1);
	strncpy(ev.to_node, node, sizeof(ev.to_node) - 1);
	ev.ts = ts;
	ev.from_band = !strcmp(from_band, "5") ? BAND_HIGH : (!strcmp(from_band, "2.4") ? BAND_LOW : 0xff);
	ev.to_band = !strcmp(to_band, "5") ? BAND_HIGH : (!strcmp(to_band, "2.4") ? BAND_LOW : 0xff);

	ev.type = MESH_EV_ROAM;
	for (i = 0; i < ARRAY_SIZE(type_name); i++)
		if (type_name[i] && !strcmp(type_name[i], type))
			ev.type = i;

	mesh_log_push(&ev);
}
