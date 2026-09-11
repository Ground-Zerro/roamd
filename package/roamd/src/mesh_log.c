#include <stdio.h>
#include <stdlib.h>
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

struct log_source {
	struct list_head list;
	char id[MESH_ID_MAX];
	uint32_t seq[MESH_REPORT_EVENTS];
	uint32_t ts[MESH_REPORT_EVENTS];
	unsigned int count;
	unsigned int head;
};

static struct mesh_event ring[MESH_LOG_MAX];
static unsigned int ring_count;
static unsigned int ring_head;
static uint32_t ring_seq;
static bool ring_dirty;
static struct uloop_timeout flush_timer;
static LIST_HEAD(sources);

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

static uint8_t band_parse(struct blob_attr *attr)
{
	const char *s = attr ? blobmsg_get_string(attr) : "";

	if (!strcmp(s, "5"))
		return BAND_HIGH;
	if (!strcmp(s, "2.4"))
		return BAND_LOW;
	return MESH_BAND_NA;
}

static struct log_source *source_find(const char *id)
{
	struct log_source *src;

	list_for_each_entry(src, &sources, list)
		if (!strcmp(src->id, id))
			return src;

	return NULL;
}

static struct log_source *source_get(const char *id)
{
	struct log_source *src = source_find(id);

	if (src)
		return src;

	src = calloc(1, sizeof(*src));
	if (!src)
		return NULL;

	strncpy(src->id, id, sizeof(src->id) - 1);
	list_add_tail(&src->list, &sources);

	return src;
}

static bool source_has(const struct log_source *src, uint32_t seq, uint32_t ts)
{
	unsigned int i;

	for (i = 0; i < src->count; i++)
		if (src->seq[i] == seq && src->ts[i] == ts)
			return true;

	return false;
}

static void source_add(struct log_source *src, uint32_t seq, uint32_t ts)
{
	unsigned int idx = (src->head + src->count) % MESH_REPORT_EVENTS;

	src->seq[idx] = seq;
	src->ts[idx] = ts;

	if (src->count < MESH_REPORT_EVENTS)
		src->count++;
	else
		src->head = (src->head + 1) % MESH_REPORT_EVENTS;
}

void mesh_log_forget(const char *id)
{
	struct log_source *src = source_find(id);

	if (!src)
		return;

	list_del(&src->list);
	free(src);
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
		"\"to_node\":\"%s\",\"from_band\":%u,\"to_band\":%u,\"type\":%u,"
		"\"origin\":\"%s\",\"origin_seq\":%u}\n",
		ev->seq, ev->ts, ev->mac, ev->from_node, ev->to_node,
		ev->from_band, ev->to_band, ev->type, ev->origin, ev->origin_seq);
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

enum {
	LF_SEQ,
	LF_TS,
	LF_MAC,
	LF_FN,
	LF_TN,
	LF_FB,
	LF_TB,
	LF_TYPE,
	LF_ORIGIN,
	LF_ORIGIN_SEQ,
	__LF_MAX
};

static const struct blobmsg_policy lf_policy[__LF_MAX] = {
	[LF_SEQ] = { .name = "seq", .type = BLOBMSG_TYPE_INT32 },
	[LF_TS] = { .name = "ts", .type = BLOBMSG_TYPE_INT32 },
	[LF_MAC] = { .name = "mac", .type = BLOBMSG_TYPE_STRING },
	[LF_FN] = { .name = "from_node", .type = BLOBMSG_TYPE_STRING },
	[LF_TN] = { .name = "to_node", .type = BLOBMSG_TYPE_STRING },
	[LF_FB] = { .name = "from_band", .type = BLOBMSG_TYPE_INT32 },
	[LF_TB] = { .name = "to_band", .type = BLOBMSG_TYPE_INT32 },
	[LF_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_INT32 },
	[LF_ORIGIN] = { .name = "origin", .type = BLOBMSG_TYPE_STRING },
	[LF_ORIGIN_SEQ] = { .name = "origin_seq", .type = BLOBMSG_TYPE_INT32 },
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
		ev.from_band = tb[LF_FB] ? blobmsg_get_u32(tb[LF_FB]) : MESH_BAND_NA;
		ev.to_band = tb[LF_TB] ? blobmsg_get_u32(tb[LF_TB]) : MESH_BAND_NA;
		ev.type = tb[LF_TYPE] ? blobmsg_get_u32(tb[LF_TYPE]) : 0;
		if (tb[LF_ORIGIN])
			strncpy(ev.origin, blobmsg_get_string(tb[LF_ORIGIN]), sizeof(ev.origin) - 1);
		ev.origin_seq = tb[LF_ORIGIN_SEQ] ? blobmsg_get_u32(tb[LF_ORIGIN_SEQ]) : 0;

		ring_insert(&ev);
		if (ev.seq > ring_seq)
			ring_seq = ev.seq;

		if (ev.origin[0]) {
			struct log_source *src = source_get(ev.origin);

			if (src)
				source_add(src, ev.origin_seq, ev.ts);
		}
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

static void event_merge(const char *id, const char *node, struct blob_attr **tb)
{
	struct mesh_event ev;
	size_t i;

	memset(&ev, 0, sizeof(ev));
	strncpy(ev.mac, blobmsg_get_string(tb[EVF_MAC]), sizeof(ev.mac) - 1);
	strncpy(ev.from_node, node, sizeof(ev.from_node) - 1);
	strncpy(ev.to_node, node, sizeof(ev.to_node) - 1);
	strncpy(ev.origin, id, sizeof(ev.origin) - 1);
	ev.origin_seq = blobmsg_get_u32(tb[EVF_SEQ]);
	ev.ts = blobmsg_get_u32(tb[EVF_TS]);
	ev.from_band = band_parse(tb[EVF_FROM_BAND]);
	ev.to_band = band_parse(tb[EVF_TO_BAND]);

	ev.type = MESH_EV_ROAM;
	for (i = 0; tb[EVF_TYPE] && i < ARRAY_SIZE(type_name); i++)
		if (type_name[i] && !strcmp(type_name[i], blobmsg_get_string(tb[EVF_TYPE])))
			ev.type = i;

	mesh_log_push(&ev);
}

void mesh_log_ingest(const char *id, const char *node, struct blob_attr *events)
{
	struct blob_attr *batch[MESH_REPORT_EVENTS];
	struct log_source *src = source_get(id);
	struct blob_attr *cur;
	unsigned int count = 0;
	int rem;

	if (!src)
		return;

	blobmsg_for_each_attr(cur, events, rem) {
		if (count == MESH_REPORT_EVENTS)
			break;
		batch[count++] = cur;
	}

	while (count--) {
		struct blob_attr *tb[__EVF_MAX];
		uint32_t seq, ts;

		blobmsg_parse(ev_policy, __EVF_MAX, tb, blobmsg_data(batch[count]),
			      blobmsg_data_len(batch[count]));
		if (!tb[EVF_SEQ] || !tb[EVF_TS] || !tb[EVF_MAC])
			continue;

		seq = blobmsg_get_u32(tb[EVF_SEQ]);
		ts = blobmsg_get_u32(tb[EVF_TS]);
		if (source_has(src, seq, ts))
			continue;

		source_add(src, seq, ts);
		event_merge(id, node, tb);
	}
}
