#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uclient.h>

#include "roamd.h"
#include "mesh.h"

#define PKG_CERT	"/etc/roamd/pkg.crt"
#define SYS_CERTS	"/etc/ssl/certs/ca-certificates.crt"
#define PKG_DIR		"/var/run/roamd/pkg"
#define PKG_TIMEOUT	20000
#define SYNC_WAIT_STEP	500
#define SYNC_WAIT_MAX	300000
#define PKG_INDEX_MAX	1048576
#define PKG_CACHE_TTL	300000
#define PKG_REDIRECT_MAX	5

struct pkg_fetch {
	struct uloop_timeout finish;
	struct uclient *cl;
	struct ustream_ssl_ctx *ssl;
	const struct ustream_ssl_ops *ops;
	char *buf;
	size_t len;
	FILE *out;
	unsigned int redirects;
	bool ok;
	mesh_pkg_cb cb;
	void *priv;
};

struct pkg_cache {
	char branch[MESH_WORD_MAX];
	char arch[MESH_ARCH_MAX];
	char name[MESH_NAME_MAX];
	struct mesh_pkg_meta meta;
	uint64_t taken;
	bool valid;
};

static struct pkg_cache cache[8];

static bool pkg_url(char *out, size_t len)
{
	if (!mesh.pkg_url[0])
		return false;

	if (strncmp(mesh.pkg_url, "https://", 8))
		return false;

	snprintf(out, len, "%s", mesh.pkg_url);

	return true;
}

bool mesh_pkg_feed_url(const char *branch, const char *arch, char *out, size_t len)
{
	char tmpl[MESH_URL_MAX];
	const char *p;
	size_t used = 0;

	if (!pkg_url(tmpl, sizeof(tmpl)))
		return false;

	if (!strstr(tmpl, "%b") && !strstr(tmpl, "%a")) {
		snprintf(out, len, "%s/%s/%s", tmpl, branch, arch);

		return true;
	}

	for (p = tmpl; *p && used + 1 < len; p++) {
		const char *sub = NULL;

		if (p[0] == '%' && p[1] == 'b')
			sub = branch;
		else if (p[0] == '%' && p[1] == 'a')
			sub = arch;

		if (!sub) {
			out[used++] = *p;
			continue;
		}

		used += snprintf(out + used, len - used, "%s", sub);
		p++;
	}

	out[used < len ? used : len - 1] = 0;

	return true;
}

static void fetch_free(struct pkg_fetch *f)
{
	if (f->cl)
		uclient_free(f->cl);

	if (f->ssl && f->ops)
		f->ops->context_free(f->ssl);

	if (f->out)
		fclose(f->out);

	free(f->buf);
	free(f);
}

static void finish_cb(struct uloop_timeout *t)
{
	struct pkg_fetch *f = container_of(t, struct pkg_fetch, finish);
	mesh_pkg_cb cb = f->cb;
	void *priv = f->priv;
	bool ok = f->ok;
	char *buf = f->buf;
	size_t len = f->len;

	f->buf = NULL;

	if (f->cl)
		uclient_free(f->cl);
	if (f->ssl && f->ops)
		f->ops->context_free(f->ssl);
	if (f->out)
		fclose(f->out);

	free(f);

	if (cb)
		cb(priv, ok, buf, len);

	free(buf);
}

static void fetch_done(struct pkg_fetch *f, bool ok)
{
	f->ok = ok;
	uloop_timeout_set(&f->finish, 0);
}

static void read_cb(struct uclient *cl)
{
	struct pkg_fetch *f = cl->priv;
	char buf[4096];
	int len;

	while ((len = uclient_read(cl, buf, sizeof(buf))) > 0) {
		if (f->out) {
			if (fwrite(buf, 1, len, f->out) != (size_t)len)
				return;

			f->len += len;
			continue;
		}

		if (f->len + len + 1 > PKG_INDEX_MAX)
			return;

		f->buf = realloc(f->buf, f->len + len + 1);
		if (!f->buf)
			return;

		memcpy(f->buf + f->len, buf, len);
		f->len += len;
		f->buf[f->len] = 0;
	}
}

static void header_done_cb(struct uclient *cl)
{
	struct pkg_fetch *f = cl->priv;

	if (f->redirects >= PKG_REDIRECT_MAX)
		return;

	if (uclient_http_redirect(cl))
		f->redirects++;
}

static void eof_cb(struct uclient *cl)
{
	struct pkg_fetch *f = cl->priv;

	fetch_done(f, cl->status_code == 200);
}

static void error_cb(struct uclient *cl, int code)
{
	roam_log(ROAM_L_DEBUG, "mesh: repository request failed: %s", uclient_strerror(code));
	fetch_done(cl->priv, false);
}

static const struct uclient_cb fetch_cb = {
	.data_read = read_cb,
	.data_eof = eof_cb,
	.header_done = header_done_cb,
	.error = error_cb,
};

static bool fetch_add_ca(struct pkg_fetch *f, const char *path)
{
	if (access(path, R_OK))
		return false;

	f->ops->context_add_ca_crt_file(f->ssl, path);

	return true;
}

bool mesh_pkg_get(const char *url, const char *path, mesh_pkg_cb cb, void *priv)
{
	struct pkg_fetch *f = calloc(1, sizeof(*f));
	bool verify;

	if (!f)
		return false;

	f->cb = cb;
	f->priv = priv;
	f->finish.cb = finish_cb;

	if (path) {
		f->out = fopen(path, "w");
		if (!f->out)
			goto fail;
	}

	f->ssl = mesh_ssl_context(&f->ops);
	if (!f->ssl)
		goto fail;

	verify = fetch_add_ca(f, SYS_CERTS);

	if (fetch_add_ca(f, PKG_CERT))
		verify = true;

	f->cl = uclient_new(url, NULL, &fetch_cb);
	if (!f->cl)
		goto fail;

	f->cl->priv = f;
	uclient_http_set_ssl_ctx(f->cl, f->ops, f->ssl, verify);
	uclient_set_timeout(f->cl, PKG_TIMEOUT);

	if (uclient_connect(f->cl) || uclient_http_set_request_type(f->cl, "GET") ||
	    uclient_request(f->cl))
		goto fail;

	return true;

fail:
	f->cb = NULL;
	fetch_free(f);

	return false;
}

static struct pkg_cache *cache_slot(const char *branch, const char *arch, const char *name)
{
	struct pkg_cache *oldest = &cache[0];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cache); i++) {
		struct pkg_cache *c = &cache[i];

		if (!strcmp(c->branch, branch) && !strcmp(c->arch, arch) && !strcmp(c->name, name))
			return c;

		if (c->taken < oldest->taken)
			oldest = c;
	}

	memset(oldest, 0, sizeof(*oldest));
	snprintf(oldest->branch, sizeof(oldest->branch), "%s", branch);
	snprintf(oldest->arch, sizeof(oldest->arch), "%s", arch);
	snprintf(oldest->name, sizeof(oldest->name), "%s", name);

	return oldest;
}

static bool opkg_meta(const char *index, const char *name, struct mesh_pkg_meta *out)
{
	const char *line = index;
	bool in_pkg = false;

	memset(out, 0, sizeof(*out));

	while (line && *line) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		char buf[256];

		snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) ? len : sizeof(buf) - 1),
			 line);

		if (!strncmp(buf, "Package: ", 9))
			in_pkg = !strcmp(buf + 9, name);
		else if (!buf[0])
			in_pkg = false;
		else if (in_pkg && !strncmp(buf, "Version: ", 9))
			snprintf(out->version, sizeof(out->version), "%.*s",
				 (int)sizeof(out->version) - 1, buf + 9);
		else if (in_pkg && !strncmp(buf, "Filename: ", 10))
			snprintf(out->file, sizeof(out->file), "%.*s",
				 (int)sizeof(out->file) - 1, buf + 10);
		else if (in_pkg && !strncmp(buf, "SHA256sum: ", 11))
			snprintf(out->sum, sizeof(out->sum), "%.*s",
				 (int)sizeof(out->sum) - 1, buf + 11);

		line = end ? end + 1 : NULL;
	}

	return out->file[0] && out->sum[0];
}

struct index_job {
	struct pkg_cache *slot;
	char url[MESH_URL_MAX];
	char name[MESH_NAME_MAX];
	bool adb;
	mesh_pkg_ready cb;
	void *priv;
};

static void index_done(struct index_job *job, bool ok)
{
	mesh_pkg_ready cb = job->cb;
	void *priv = job->priv;

	job->slot->valid = ok;
	job->slot->taken = roam_now;
	free(job);

	if (cb)
		cb(priv, ok);
}

static void adb_ready(void *priv, bool ok, char *data, size_t len)
{
	struct index_job *job = priv;
	char path[64];
	FILE *f;

	if (!ok || !data || !len) {
		index_done(job, false);
		return;
	}

	snprintf(path, sizeof(path), "/tmp/roamd-index.adb");
	f = fopen(path, "w");
	if (f) {
		ok = fwrite(data, 1, len, f) == len;
		fclose(f);
	}

	if (ok)
		ok = apk_index_meta(path, job->name, &job->slot->meta);

	unlink(path);
	index_done(job, ok);
}

static void packages_ready(void *priv, bool ok, char *data, size_t len)
{
	struct index_job *job = priv;
	char url[MESH_URL_MAX + 16];

	if (ok && data && len && opkg_meta(data, job->name, &job->slot->meta)) {
		job->slot->meta.adb = false;
		index_done(job, true);

		return;
	}

	snprintf(url, sizeof(url), "%s/packages.adb", job->url);
	job->slot->meta.adb = true;

	if (!mesh_pkg_get(url, NULL, adb_ready, job))
		index_done(job, false);
}

bool mesh_pkg_refresh(const char *branch, const char *arch, const char *name,
		      mesh_pkg_ready cb, void *priv)
{
	struct index_job *job;
	char url[MESH_URL_MAX + 16];

	job = calloc(1, sizeof(*job));
	if (!job)
		return false;

	job->slot = cache_slot(branch, arch, name);
	job->cb = cb;
	job->priv = priv;
	snprintf(job->name, sizeof(job->name), "%s", name);

	if (!mesh_pkg_feed_url(branch, arch, job->url, sizeof(job->url))) {
		free(job);

		return false;
	}

	snprintf(url, sizeof(url), "%s/Packages", job->url);

	if (!mesh_pkg_get(url, NULL, packages_ready, job)) {
		free(job);

		return false;
	}

	return true;
}

bool mesh_pkg_known(const char *branch, const char *arch, const char *name,
		    struct mesh_pkg_meta *out)
{
	struct pkg_cache *c = cache_slot(branch, arch, name);

	if (!c->valid)
		return false;

	if (out)
		*out = c->meta;

	return true;
}

bool mesh_pkg_stale(const char *branch, const char *arch, const char *name)
{
	struct pkg_cache *c = cache_slot(branch, arch, name);

	return !c->taken || roam_now - c->taken > PKG_CACHE_TTL;
}

static bool pkg_digest_ok(const char *path, const struct mesh_pkg_meta *meta)
{
	char hex[65];

	if (meta->adb) {
		if (!apk_block_digest(path, hex, sizeof(hex)))
			return false;
	} else if (!roam_sha256_file(path, hex, sizeof(hex))) {
		return false;
	}

	return !strcasecmp(hex, meta->sum);
}

bool mesh_pkg_path(const char *branch, const char *arch, const char *name,
		   char *out, size_t len)
{
	struct mesh_pkg_meta meta;

	if (!mesh_pkg_known(branch, arch, name, &meta))
		return false;

	snprintf(out, len, PKG_DIR "/%s/%s/%s", branch, arch, meta.file);

	return !access(out, R_OK) && pkg_digest_ok(out, &meta);
}

struct pkg_job {
	char path[256];
	struct mesh_pkg_meta meta;
	mesh_pkg_ready cb;
	void *priv;
};

static void download_ready(void *priv, bool ok, char *data, size_t len)
{
	struct pkg_job *job = priv;

	ok = ok && pkg_digest_ok(job->path, &job->meta);

	if (!ok)
		unlink(job->path);

	if (job->cb)
		job->cb(job->priv, ok);

	free(job);
}

bool mesh_pkg_download(const char *branch, const char *arch, const char *name,
		       mesh_pkg_ready cb, void *priv)
{
	char url[MESH_URL_MAX + MESH_NAME_MAX + 8], dir[224];
	struct pkg_job *job;

	job = calloc(1, sizeof(*job));
	if (!job)
		return false;

	job->cb = cb;
	job->priv = priv;

	if (!mesh_pkg_known(branch, arch, name, &job->meta) ||
	    !mesh_pkg_feed_url(branch, arch, url, sizeof(url))) {
		free(job);

		return false;
	}

	snprintf(dir, sizeof(dir), PKG_DIR "/%s/%s", branch, arch);
	mesh_dir_ensure(PKG_DIR);
	{
		char part[224];
		char *slash;

		snprintf(part, sizeof(part), "%s", dir);
		slash = strrchr(part, '/');
		if (slash) {
			*slash = 0;
			mkdir(part, 0755);
		}
	}
	mkdir(dir, 0755);

	snprintf(job->path, sizeof(job->path), "%s/%s", dir, job->meta.file);

	if (pkg_digest_ok(job->path, &job->meta)) {
		if (cb)
			cb(priv, true);

		free(job);

		return true;
	}

	snprintf(url + strlen(url), sizeof(url) - strlen(url), "/%s", job->meta.file);

	if (!mesh_pkg_get(url, job->path, download_ready, job)) {
		free(job);

		return false;
	}

	return true;
}

struct sync_state {
	bool ok;
	bool done;
};

static bool sync_wait(struct sync_state *s)
{
	int waited = 0;

	while (!s->done && waited < SYNC_WAIT_MAX) {
		uloop_run_timeout(SYNC_WAIT_STEP);
		waited += SYNC_WAIT_STEP;
	}

	return s->done && s->ok;
}

static void probe_ready(void *priv, bool ok, char *data, size_t len)
{
	struct sync_state *s = priv;

	s->ok = ok;
	s->done = true;
	uloop_end();
}

bool mesh_pkg_probe(const char *url)
{
	struct sync_state s = { 0 };

	if (!mesh_pkg_get(url, "/dev/null", probe_ready, &s))
		return false;

	return sync_wait(&s);
}

static void sync_cb(void *priv, bool ok)
{
	struct sync_state *s = priv;

	s->ok = ok;
	s->done = true;
	uloop_end();
}

bool mesh_pkg_sync(const char *branch, const char *arch, const char *name)
{
	struct sync_state s = { 0 };

	if (!mesh_pkg_refresh(branch, arch, name, sync_cb, &s))
		return false;

	if (!sync_wait(&s))
		return false;

	memset(&s, 0, sizeof(s));

	if (!mesh_pkg_download(branch, arch, name, sync_cb, &s))
		return false;

	return sync_wait(&s);
}

int mesh_pkg_newer(const char *have, const char *want)
{
	unsigned long a, b;
	const char *pa = have, *pb = want;

	if (!have || !want || !have[0] || !want[0])
		return 0;

	while (*pa || *pb) {
		char *ea, *eb;

		a = strtoul(pa, &ea, 10);
		b = strtoul(pb, &eb, 10);

		if (a != b)
			return b > a ? 1 : -1;

		if (*ea != *eb)
			return strcmp(eb, ea) > 0 ? 1 : -1;

		if (!*ea)
			break;

		pa = ea + 1;
		pb = eb + 1;
	}

	return 0;
}
