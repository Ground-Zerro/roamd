#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>

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

#define DEFLATE_MAXBITS		15
#define DEFLATE_MAXLCODES	286
#define DEFLATE_MAXDCODES	30
#define DEFLATE_FIXLCODES	288
#define DEFLATE_OUT_MIN		65536

struct adb {
	const uint8_t *ptr;
	size_t len;
};

struct deflate {
	const uint8_t *in;
	size_t in_len;
	size_t in_pos;
	uint32_t bitbuf;
	int bitcnt;
	uint8_t *out;
	size_t out_len;
	size_t out_cap;
};

struct huffman {
	short count[DEFLATE_MAXBITS + 1];
	short symbol[DEFLATE_FIXLCODES];
};

static const short length_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const short length_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const short dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const short dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
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

static int deflate_bits(struct deflate *d, int need)
{
	uint32_t val = d->bitbuf;

	while (d->bitcnt < need) {
		if (d->in_pos == d->in_len)
			return -1;
		val |= (uint32_t)d->in[d->in_pos++] << d->bitcnt;
		d->bitcnt += 8;
	}

	d->bitbuf = val >> need;
	d->bitcnt -= need;

	return val & ((1u << need) - 1);
}

static bool deflate_reserve(struct deflate *d, size_t n)
{
	size_t cap = d->out_cap ? d->out_cap : DEFLATE_OUT_MIN;
	uint8_t *grown;

	if (d->out_len + n <= d->out_cap)
		return true;

	while (cap < d->out_len + n)
		cap *= 2;

	if (cap > APK_FILE_MAX)
		return false;

	grown = realloc(d->out, cap);
	if (!grown)
		return false;

	d->out = grown;
	d->out_cap = cap;

	return true;
}

static int huffman_build(struct huffman *h, const short *length, int n)
{
	short offs[DEFLATE_MAXBITS + 1];
	int symbol, len, left = 1;

	memset(h->count, 0, sizeof(h->count));

	for (symbol = 0; symbol < n; symbol++)
		h->count[length[symbol]]++;

	if (h->count[0] == n)
		return 0;

	for (len = 1; len <= DEFLATE_MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}

	offs[1] = 0;
	for (len = 1; len < DEFLATE_MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];

	for (symbol = 0; symbol < n; symbol++)
		if (length[symbol])
			h->symbol[offs[length[symbol]]++] = symbol;

	return left;
}

static int huffman_decode(struct deflate *d, const struct huffman *h)
{
	int code = 0, first = 0, index = 0, len, bit;

	for (len = 1; len <= DEFLATE_MAXBITS; len++) {
		bit = deflate_bits(d, 1);
		if (bit < 0)
			return -1;

		code |= bit;
		if (code - h->count[len] < first)
			return h->symbol[index + (code - first)];

		index += h->count[len];
		first = (first + h->count[len]) << 1;
		code <<= 1;
	}

	return -1;
}

static int deflate_stored(struct deflate *d)
{
	const uint8_t *p = d->in + d->in_pos;
	unsigned len;

	d->bitbuf = 0;
	d->bitcnt = 0;

	if (d->in_len - d->in_pos < 4)
		return -1;

	len = p[0] | p[1] << 8;
	if (p[2] != (~len & 0xff) || p[3] != ((~len >> 8) & 0xff))
		return -1;

	d->in_pos += 4;
	if (d->in_len - d->in_pos < len || !deflate_reserve(d, len))
		return -1;

	memcpy(d->out + d->out_len, d->in + d->in_pos, len);
	d->out_len += len;
	d->in_pos += len;

	return 0;
}

static int deflate_codes(struct deflate *d, const struct huffman *lencode,
			 const struct huffman *distcode)
{
	int symbol, extra;
	size_t len, dist;

	for (;;) {
		symbol = huffman_decode(d, lencode);
		if (symbol < 0)
			return -1;

		if (symbol == 256)
			return 0;

		if (symbol < 256) {
			if (!deflate_reserve(d, 1))
				return -1;
			d->out[d->out_len++] = symbol;
			continue;
		}

		symbol -= 257;
		if (symbol >= 29 || (extra = deflate_bits(d, length_extra[symbol])) < 0)
			return -1;
		len = length_base[symbol] + extra;

		symbol = huffman_decode(d, distcode);
		if (symbol < 0 || symbol >= 30 || (extra = deflate_bits(d, dist_extra[symbol])) < 0)
			return -1;
		dist = dist_base[symbol] + extra;

		if (dist > d->out_len || !deflate_reserve(d, len))
			return -1;

		while (len--) {
			d->out[d->out_len] = d->out[d->out_len - dist];
			d->out_len++;
		}
	}
}

static int deflate_fixed(struct deflate *d)
{
	struct huffman lencode, distcode;
	short lengths[DEFLATE_FIXLCODES];
	int symbol;

	for (symbol = 0; symbol < 144; symbol++)
		lengths[symbol] = 8;
	for (; symbol < 256; symbol++)
		lengths[symbol] = 9;
	for (; symbol < 280; symbol++)
		lengths[symbol] = 7;
	for (; symbol < DEFLATE_FIXLCODES; symbol++)
		lengths[symbol] = 8;
	huffman_build(&lencode, lengths, DEFLATE_FIXLCODES);

	for (symbol = 0; symbol < DEFLATE_MAXDCODES; symbol++)
		lengths[symbol] = 5;
	huffman_build(&distcode, lengths, DEFLATE_MAXDCODES);

	return deflate_codes(d, &lencode, &distcode);
}

static int deflate_dynamic(struct deflate *d)
{
	static const short order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	short lengths[DEFLATE_MAXLCODES + DEFLATE_MAXDCODES];
	struct huffman lencode, distcode;
	int nlen, ndist, ncode, index, symbol, repeat, err;
	short len;

	nlen = deflate_bits(d, 5);
	ndist = deflate_bits(d, 5);
	ncode = deflate_bits(d, 4);
	if (nlen < 0 || ndist < 0 || ncode < 0)
		return -1;

	nlen += 257;
	ndist += 1;
	ncode += 4;
	if (nlen > DEFLATE_MAXLCODES || ndist > DEFLATE_MAXDCODES)
		return -1;

	for (index = 0; index < 19; index++) {
		symbol = index < ncode ? deflate_bits(d, 3) : 0;
		if (symbol < 0)
			return -1;
		lengths[order[index]] = symbol;
	}

	if (huffman_build(&lencode, lengths, 19))
		return -1;

	for (index = 0; index < nlen + ndist; ) {
		symbol = huffman_decode(d, &lencode);
		if (symbol < 0)
			return -1;

		if (symbol < 16) {
			lengths[index++] = symbol;
			continue;
		}

		len = 0;
		if (symbol == 16) {
			if (!index)
				return -1;
			len = lengths[index - 1];
			repeat = deflate_bits(d, 2) + 3;
		} else if (symbol == 17) {
			repeat = deflate_bits(d, 3) + 3;
		} else {
			repeat = deflate_bits(d, 7) + 11;
		}

		if (repeat < 3 || index + repeat > nlen + ndist)
			return -1;

		while (repeat--)
			lengths[index++] = len;
	}

	if (!lengths[256])
		return -1;

	err = huffman_build(&lencode, lengths, nlen);
	if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
		return -1;

	err = huffman_build(&distcode, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
		return -1;

	return deflate_codes(d, &lencode, &distcode);
}

static uint8_t *adb_inflate(const uint8_t *in, size_t in_len, size_t *out_len)
{
	struct deflate d = { .in = in + 4, .in_len = in_len - 4 };
	int last, type, err;
	uint8_t *out;

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

	do {
		last = deflate_bits(&d, 1);
		type = deflate_bits(&d, 2);

		if (last < 0 || type < 0)
			err = -1;
		else if (type == 0)
			err = deflate_stored(&d);
		else if (type == 1)
			err = deflate_fixed(&d);
		else if (type == 2)
			err = deflate_dynamic(&d);
		else
			err = -1;
	} while (!err && !last);

	if (err) {
		free(d.out);
		return NULL;
	}

	*out_len = d.out_len;
	return d.out;
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
