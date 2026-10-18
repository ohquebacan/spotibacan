#include "proto3ds.h"

#include <string.h>

void pb_init(pb *b, uint8_t *storage, size_t cap)
{
	b->p = storage;
	b->len = 0;
	b->cap = cap;
	b->overflow = false;
}

bool pb_ok(const pb *b) { return !b->overflow; }

static bool put(pb *b, const void *data, size_t n)
{
	if (b->overflow || b->len + n > b->cap) {
		b->overflow = true;
		return false;
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
	return true;
}

static bool put_varint(pb *b, uint64_t v)
{
	uint8_t tmp[10];
	size_t n = 0;
	do {
		uint8_t byte = (uint8_t)(v & 0x7F);
		v >>= 7;
		tmp[n++] = byte | (v ? 0x80 : 0);
	} while (v);
	return put(b, tmp, n);
}

/* Wire type 0 for varints, 2 for everything length-delimited. */
static bool put_tag(pb *b, uint32_t field, unsigned wire)
{
	return put_varint(b, ((uint64_t)field << 3) | wire);
}

bool pb_varint(pb *b, uint32_t field, uint64_t v)
{
	return put_tag(b, field, 0) && put_varint(b, v);
}

bool pb_bytes(pb *b, uint32_t field, const void *data, size_t n)
{
	return put_tag(b, field, 2) && put_varint(b, n) && put(b, data, n);
}

bool pb_str(pb *b, uint32_t field, const char *s)
{
	return pb_bytes(b, field, s, strlen(s));
}

bool pb_sub(pb *b, uint32_t field, const pb *sub)
{
	if (sub->overflow) {
		b->overflow = true;
		return false;
	}
	return pb_bytes(b, field, sub->p, sub->len);
}

/* ------------------------------------------------------------------ reading */

static bool get_varint(const uint8_t *buf, size_t len, size_t *at, uint64_t *out)
{
	uint64_t v = 0;
	unsigned shift = 0;
	while (*at < len) {
		uint8_t b = buf[(*at)++];
		v |= (uint64_t)(b & 0x7F) << shift;
		if (!(b & 0x80)) {
			*out = v;
			return true;
		}
		shift += 7;
		if (shift > 63)
			return false;
	}
	return false;
}

/* Walks the top level once, stopping at `field`. Repeated fields yield the
 * first occurrence, which is all this client needs. */
static bool scan(const uint8_t *buf, size_t len, uint32_t field,
                 unsigned want_wire, pbv *bytes_out, uint64_t *num_out)
{
	size_t at = 0;
	while (at < len) {
		uint64_t key;
		if (!get_varint(buf, len, &at, &key))
			return false;
		uint32_t f = (uint32_t)(key >> 3);
		unsigned wire = (unsigned)(key & 7);

		switch (wire) {
		case 0: {
			uint64_t v;
			if (!get_varint(buf, len, &at, &v))
				return false;
			if (f == field && want_wire == 0) {
				if (num_out)
					*num_out = v;
				return true;
			}
			break;
		}
		case 2: {
			uint64_t n;
			if (!get_varint(buf, len, &at, &n) || n > len - at)
				return false;
			if (f == field && want_wire == 2) {
				if (bytes_out) {
					bytes_out->p = buf + at;
					bytes_out->len = (size_t)n;
				}
				return true;
			}
			at += (size_t)n;
			break;
		}
		case 5: if (len - at < 4) return false; at += 4; break;
		case 1: if (len - at < 8) return false; at += 8; break;
		default: return false;     /* groups: not used by these messages */
		}
	}
	return false;
}

bool pb_get_bytes(const uint8_t *buf, size_t len, uint32_t field, pbv *out)
{
	return scan(buf, len, field, 2, out, NULL);
}

bool pb_get_varint(const uint8_t *buf, size_t len, uint32_t field, uint64_t *out)
{
	return scan(buf, len, field, 0, NULL, out);
}

bool pb_get_sint(const uint8_t *buf, size_t len, uint32_t field, int64_t *out)
{
	uint64_t raw;
	if (!scan(buf, len, field, 0, NULL, &raw))
		return false;
	if (out)
		*out = (int64_t)(raw >> 1) ^ -(int64_t)(raw & 1);
	return true;
}

bool pb_has(const uint8_t *buf, size_t len, uint32_t field)
{
	pbv v;
	uint64_t n;
	return scan(buf, len, field, 2, &v, NULL) ||
	       scan(buf, len, field, 0, NULL, &n);
}

bool pb_dig(const uint8_t *buf, size_t len, const uint32_t *fields, size_t depth,
            pbv *out)
{
	pbv cur = { buf, len };
	for (size_t i = 0; i < depth; i++) {
		pbv next;
		if (!pb_get_bytes(cur.p, cur.len, fields[i], &next))
			return false;
		cur = next;
	}
	*out = cur;
	return true;
}
