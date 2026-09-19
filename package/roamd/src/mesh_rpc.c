#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uclient.h>

#include "roamd.h"
#include "mesh.h"

#define RPC_TIMEOUT	12000
#define RPC_SESSION_TTL	240000
#define RPC_NULL	"00000000000000000000000000000000"
#define RPC_BODY_MAX	16384
#define RPC_RESP_MAX	262144
#define MEMBERS_CERT	"/etc/roamd/members/%s.crt"
#define MEMBERS_TOKEN	"/etc/roamd/tokens/%s"

struct rpc_session {
	char id[MESH_ID_MAX];
	char sid[40];
	uint64_t taken;
};

struct rpc_req {
	struct uloop_timeout finish;
	struct blob_attr *result;
	bool ok;
	struct uclient *cl;
	struct ustream_ssl_ctx *ssl;
	const struct ustream_ssl_ops *ops;
	char id[MESH_ID_MAX];
	char addr[MESH_ADDR_MAX];
	char object[24];
	char method[24];
	char *args;
	char *resp;
	size_t resp_len;
	mesh_rpc_cb cb;
	void *priv;
	bool login;
	bool retried;
	bool retry_now;
	bool relogin;
};

static struct rpc_session sessions[MESH_ALLOW_MAX];

static struct rpc_session *session_slot(const char *id, bool create)
{
	struct rpc_session *free_slot = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sessions); i++) {
		if (!strcmp(sessions[i].id, id))
			return &sessions[i];

		if (!free_slot && !sessions[i].id[0])
			free_slot = &sessions[i];
	}

	if (!create || !free_slot)
		return NULL;

	snprintf(free_slot->id, sizeof(free_slot->id), "%s", id);
	free_slot->sid[0] = 0;

	return free_slot;
}

static const char *session_get(const char *id)
{
	struct rpc_session *s = session_slot(id, false);

	if (!s || !s->sid[0] || roam_now - s->taken > RPC_SESSION_TTL)
		return NULL;

	return s->sid;
}

static void session_set(const char *id, const char *sid)
{
	struct rpc_session *s = session_slot(id, true);

	if (!s)
		return;

	snprintf(s->sid, sizeof(s->sid), "%s", sid ? sid : "");
	s->taken = roam_now;
}

void mesh_rpc_forget(const char *id)
{
	struct rpc_session *s = session_slot(id, false);

	if (s)
		s->id[0] = 0;
}

static void conn_drop(struct rpc_req *r)
{
	if (r->cl) {
		uclient_free(r->cl);
		r->cl = NULL;
	}

	if (r->ssl && r->ops) {
		r->ops->context_free(r->ssl);
		r->ssl = NULL;
	}

	free(r->resp);
	r->resp = NULL;
	r->resp_len = 0;
}

static bool req_start(struct rpc_req *r);

static void finish_cb(struct uloop_timeout *t)
{
	struct rpc_req *r = container_of(t, struct rpc_req, finish);
	bool again = false;

	conn_drop(r);

	if (r->retry_now) {
		r->retry_now = false;
		r->retried = true;
		r->login = true;
		mesh_rpc_forget(r->id);
		again = req_start(r);

		if (again)
			return;
	}

	if (r->relogin) {
		r->relogin = false;
		r->login = false;

		if (req_start(r))
			return;
	}

	if (r->cb)
		r->cb(r->priv, r->ok ? r->result : NULL, r->ok);

	free(r->result);
	free(r->args);
	free(r);
}

static void req_done(struct rpc_req *r, struct blob_attr *result, bool ok)
{
	free(r->result);
	r->result = result ? blob_memdup(result) : NULL;
	r->ok = ok && (!result || r->result);
	uloop_timeout_set(&r->finish, 0);
}

static void req_retry(struct rpc_req *r)
{
	r->retry_now = true;
	uloop_timeout_set(&r->finish, 0);
}

static void resp_parse(struct rpc_req *r)
{
	static struct blob_buf doc;
	struct blob_attr *cur, *result = NULL, *error = NULL;
	int rem;

	if (!r->resp_len) {
		req_done(r, NULL, false);
		return;
	}

	blob_buf_init(&doc, 0);

	if (!blobmsg_add_json_from_string(&doc, r->resp)) {
		req_done(r, NULL, false);
		return;
	}

	blob_for_each_attr(cur, doc.head, rem) {
		if (!strcmp(blobmsg_name(cur), "result"))
			result = cur;
		else if (!strcmp(blobmsg_name(cur), "error"))
			error = cur;
	}

	if (error || !result || blobmsg_type(result) != BLOBMSG_TYPE_ARRAY) {
		if (!r->retried && !r->login) {
			req_retry(r);
			return;
		}

		req_done(r, NULL, false);
		return;
	}

	blobmsg_for_each_attr(cur, result, rem) {
		if (blobmsg_type(cur) == BLOBMSG_TYPE_INT32 && blobmsg_get_u32(cur)) {
			if (!r->retried && !r->login) {
				req_retry(r);
				return;
			}

			req_done(r, NULL, false);
			return;
		}

		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE)
			continue;

		if (!r->login) {
			req_done(r, cur, true);
			return;
		}

		{
			struct blob_attr *f;
			int frem;

			blobmsg_for_each_attr(f, cur, frem) {
				if (strcmp(blobmsg_name(f), "ubus_rpc_session") ||
				    blobmsg_type(f) != BLOBMSG_TYPE_STRING)
					continue;

				session_set(r->id, blobmsg_get_string(f));
				r->relogin = true;
				uloop_timeout_set(&r->finish, 0);

				return;
			}
		}
	}

	req_done(r, NULL, !r->login);
}

static void cb_read(struct uclient *cl)
{
	struct rpc_req *r = cl->priv;
	char buf[2048];
	int len;

	while ((len = uclient_read(cl, buf, sizeof(buf))) > 0) {
		char *grown;

		if (r->resp_len + len + 1 > RPC_RESP_MAX)
			return;

		grown = realloc(r->resp, r->resp_len + len + 1);
		if (!grown)
			return;

		r->resp = grown;
		memcpy(r->resp + r->resp_len, buf, len);
		r->resp_len += len;
		r->resp[r->resp_len] = 0;
	}
}

static void cb_eof(struct uclient *cl)
{
	resp_parse(cl->priv);
}

static void cb_error(struct uclient *cl, int code)
{
	struct rpc_req *r = cl->priv;

	roam_log(ROAM_L_DEBUG, "mesh: %s of %s failed: %s", r->method, r->id,
		 uclient_strerror(code));
	req_done(r, NULL, false);
}

static const struct uclient_cb rpc_cb = {
	.data_read = cb_read,
	.data_eof = cb_eof,
	.error = cb_error,
};

static void url_of(const char *addr, char *out, size_t len)
{
	if (strchr(addr, ':'))
		snprintf(out, len, "https://[%s]/ubus", addr);
	else
		snprintf(out, len, "https://%s/ubus", addr);
}

static bool token_read(const char *id, char *out, size_t len)
{
	char path[128], *raw;

	snprintf(path, sizeof(path), MEMBERS_TOKEN, id);
	raw = mesh_slurp(path, 64);
	if (!raw)
		return false;

	snprintf(out, len, "%s", raw);
	free(raw);
	out[strcspn(out, "\r\n")] = 0;

	return out[0] != 0;
}

static bool body_build(struct rpc_req *r, char *out, size_t len)
{
	const char *sid = session_get(r->id);
	char token[64];

	if (r->login) {
		if (!token_read(r->id, token, sizeof(token)))
			return false;

		snprintf(out, len,
			 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"call\",\"params\":"
			 "[\"" RPC_NULL "\",\"session\",\"login\","
			 "{\"username\":\"mesh-%s\",\"password\":\"%s\"}]}", r->id, token);

		return true;
	}

	if (!sid)
		return false;

	snprintf(out, len,
		 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"call\",\"params\":"
		 "[\"%s\",\"%s\",\"%s\",%s]}", sid, r->object, r->method,
		 r->args && r->args[0] ? r->args : "{}");

	return true;
}

static bool req_start(struct rpc_req *r)
{
	char url[MESH_ADDR_MAX + 32], cert[128];
	char *body;
	bool ok = false;

	conn_drop(r);

	if (!session_get(r->id))
		r->login = true;

	body = malloc(RPC_BODY_MAX);
	if (!body)
		return false;

	if (!body_build(r, body, RPC_BODY_MAX))
		goto out;

	snprintf(cert, sizeof(cert), MEMBERS_CERT, r->id);
	if (access(cert, R_OK))
		goto out;

	url_of(r->addr, url, sizeof(url));

	r->ssl = mesh_ssl_context(&r->ops);
	if (!r->ssl)
		goto out;

	r->ops->context_add_ca_crt_file(r->ssl, cert);

	r->cl = uclient_new(url, NULL, &rpc_cb);
	if (!r->cl)
		goto out;

	r->cl->priv = r;
	uclient_http_set_ssl_ctx(r->cl, r->ops, r->ssl, true);
	uclient_set_timeout(r->cl, RPC_TIMEOUT);

	if (uclient_connect(r->cl) || uclient_http_set_request_type(r->cl, "POST"))
		goto out;

	uclient_http_reset_headers(r->cl);
	uclient_http_set_header(r->cl, "Content-Type", "application/json");
	uclient_write(r->cl, body, strlen(body));

	ok = !uclient_request(r->cl);

out:
	free(body);

	return ok;
}

bool mesh_rpc_call(const char *id, const char *addr, const char *object,
		   const char *method, const char *args, mesh_rpc_cb cb, void *priv)
{
	struct rpc_req *r = calloc(1, sizeof(*r));

	if (!r)
		return false;

	snprintf(r->id, sizeof(r->id), "%s", id);
	snprintf(r->addr, sizeof(r->addr), "%s", addr);
	snprintf(r->object, sizeof(r->object), "%s", object);
	snprintf(r->method, sizeof(r->method), "%s", method);
	r->args = args ? strdup(args) : NULL;
	r->cb = cb;
	r->priv = priv;

	r->finish.cb = finish_cb;

	if (req_start(r))
		return true;

	free(r->args);
	free(r);

	return false;
}
