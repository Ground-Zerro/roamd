#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <zlib.h>

#include "roamd.h"

#define APK_FILE_MAX		(64 * 1024 * 1024)

#define ADB_TYPE_MASK		0xf0000000
#define ADB_VALUE_MASK		0x0fffffff
#define ADB_TYPE_INT		0x10000000
#define ADB_TYPE_INT_32		0x20000000
#define ADB_TYPE_INT_64		0x30000000
#define ADB_TYPE_BLOB_8		0x80000000
#define ADB_TYPE_BLOB_16	0x90000000
#define ADB_TYPE_BLOB_32	0xa0000000
#define ADB_TYPE_ARRAY		0xd0000000
#define ADB_TYPE_OBJECT		0xe0000000

#define ADB_BLOCK_ADB		0
#define ADB_BLOCK_EXT		3

#define ADB_SCHEMA_INDEX	0x78646e69
#define ADB_SCHEMA_PACKAGE	0x676b6370

#define ADBI_NDX_PACKAGES	2
#define ADBI_PI_NAME		1
#define ADBI_PI_VERSION		2
#define ADBI_PI_HASHES		3
#define ADBI_PI_FILE_SIZE	13

struct adb {
	const uint8_t *ptr;
	size_t len;
};

static uint32_t le32_at(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return le32toh(v);
}

static uint64_t le64_at(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return le64toh(v);
}

static uint8_t *adb_inflate(const uint8_t *in, size_t in_len, size_t *out_len)
{
	size_t cap = in_len * 4, used = 0;
	uint8_t *out, *grown;
	int r = Z_OK;
	z_stream zs;

	if (in_len < 4 || memcmp(in, "ADB", 3))
		return NULL;

	if (in[3] == '.') {
		out = malloc(in_len);
		if (out) {
			memcpy(out, in, in_len);
			*out_len = in_len;
		}
		return out;
	}

	if (in[3] != 'd')
		return NULL;

	memset(&zs, 0, sizeof(zs));
	if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
		return NULL;

	out = malloc(cap);
	zs.next_in = (Bytef *)in + 4;
	zs.avail_in = in_len - 4;

	while (out && r == Z_OK) {
		if (used == cap) {
			grown = cap < APK_FILE_MAX ? realloc(out, cap * 2) : NULL;
			if (!grown)
				break;
			out = grown;
			cap *= 2;
		}

		zs.next_out = out + used;
		zs.avail_out = cap - used;
		r = inflate(&zs, Z_NO_FLUSH);
		used = cap - zs.avail_out;
	}

	inflateEnd(&zs);

	if (r != Z_STREAM_END) {
		free(out);
		return NULL;
	}

	*out_len = used;
	return out;
}

static uint8_t *apk_file_inflate(const char *path, size_t *len)
{
	uint8_t *buf = NULL, *raw = NULL;
	FILE *f = fopen(path, "rb");
	long size = 0;

	if (!f)
		return NULL;

	if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && size <= APK_FILE_MAX &&
	    !fseek(f, 0, SEEK_SET) && (buf = malloc(size)) &&
	    fread(buf, 1, size, f) == (size_t)size)
		raw = adb_inflate(buf, size, len);

	free(buf);
	fclose(f);

	return raw;
}

static bool adb_block(const uint8_t *raw, size_t len, uint32_t schema, struct adb *adb)
{
	uint32_t type_size;
	uint64_t size;
	size_t hdr;

	if (len < 12 || memcmp(raw, "ADB.", 4) || le32_at(raw + 4) != schema)
		return false;

	type_size = le32_at(raw + 8);

	if (type_size >> 30 == ADB_BLOCK_EXT) {
		if (len < 24 || (type_size & 0x3fffffff) != ADB_BLOCK_ADB)
			return false;
		hdr = 16;
		size = le64_at(raw + 16);
	} else {
		if (type_size >> 30 != ADB_BLOCK_ADB)
			return false;
		hdr = 4;
		size = type_size & 0x3fffffff;
	}

	if (size < hdr || size > len - 8)
		return false;

	adb->ptr = raw + 8 + hdr;
	adb->len = size - hdr;

	return adb->len >= 8;
}

static const uint8_t *adb_at(const struct adb *adb, uint32_t v, size_t offs, size_t size)
{
	offs += v & ADB_VALUE_MASK;

	if (size > adb->len || offs > adb->len - size)
		return NULL;

	return adb->ptr + offs;
}

static uint32_t adb_count(const struct adb *adb, uint32_t v)
{
	const uint8_t *o;

	if ((v & ADB_TYPE_MASK) != ADB_TYPE_OBJECT && (v & ADB_TYPE_MASK) != ADB_TYPE_ARRAY)
		return 0;

	o = adb_at(adb, v, 0, 4);

	return o ? le32_at(o) : 0;
}

static uint32_t adb_field(const struct adb *adb, uint32_t v, uint32_t i)
{
	const uint8_t *o;

	if (i >= adb_count(adb, v))
		return 0;

	o = adb_at(adb, v, 4 * (size_t)i, 4);

	return o ? le32_at(o) : 0;
}

static const uint8_t *adb_blob(const struct adb *adb, uint32_t v, size_t *n)
{
	const uint8_t *p;

	switch (v & ADB_TYPE_MASK) {
	case ADB_TYPE_BLOB_8:
		p = adb_at(adb, v, 0, 1);
		if (!p)
			return NULL;
		*n = p[0];
		return adb_at(adb, v, 1, *n);
	case ADB_TYPE_BLOB_16:
		p = adb_at(adb, v, 0, 2);
		if (!p)
			return NULL;
		*n = p[0] | p[1] << 8;
		return adb_at(adb, v, 2, *n);
	case ADB_TYPE_BLOB_32:
		p = adb_at(adb, v, 0, 4);
		if (!p)
			return NULL;
		*n = le32_at(p);
		return adb_at(adb, v, 4, *n);
	default:
		return NULL;
	}
}

static uint64_t adb_int(const struct adb *adb, uint32_t v)
{
	const uint8_t *p;

	switch (v & ADB_TYPE_MASK) {
	case ADB_TYPE_INT:
		return v & ADB_VALUE_MASK;
	case ADB_TYPE_INT_32:
		p = adb_at(adb, v, 0, 4);
		return p ? le32_at(p) : 0;
	case ADB_TYPE_INT_64:
		p = adb_at(adb, v, 0, 8);
		return p ? le64_at(p) : 0;
	default:
		return 0;
	}
}

bool apk_index_print(const char *path)
{
	size_t len, name_len = 0, ver_len = 0, hash_len = 0, j;
	const uint8_t *name, *ver, *hash;
	uint32_t packages, pkg, n, i;
	struct adb adb;
	uint8_t *raw;
	bool ok;

	raw = apk_file_inflate(path, &len);
	if (!raw)
		return false;

	ok = adb_block(raw, len, ADB_SCHEMA_INDEX, &adb);
	if (ok) {
		packages = adb_field(&adb, le32_at(adb.ptr + 4), ADBI_NDX_PACKAGES);
		n = adb_count(&adb, packages);

		for (i = 1; i < n; i++) {
			pkg = adb_field(&adb, packages, i);
			name = adb_blob(&adb, adb_field(&adb, pkg, ADBI_PI_NAME), &name_len);
			ver = adb_blob(&adb, adb_field(&adb, pkg, ADBI_PI_VERSION), &ver_len);
			hash = adb_blob(&adb, adb_field(&adb, pkg, ADBI_PI_HASHES), &hash_len);
			if (!name || !ver || !hash)
				continue;

			printf("%.*s %.*s %llu ", (int)name_len, name, (int)ver_len, ver,
			       (unsigned long long)adb_int(&adb, adb_field(&adb, pkg, ADBI_PI_FILE_SIZE)));
			for (j = 0; j < hash_len; j++)
				printf("%02x", hash[j]);
			putchar('\n');
		}
	}

	free(raw);

	return ok;
}

bool apk_block_print(const char *path)
{
	struct adb adb;
	uint8_t *raw;
	size_t len;
	bool ok;

	raw = apk_file_inflate(path, &len);
	if (!raw)
		return false;

	ok = adb_block(raw, len, ADB_SCHEMA_PACKAGE, &adb) &&
	     fwrite(adb.ptr, 1, adb.len, stdout) == adb.len;

	free(raw);

	return ok;
}
