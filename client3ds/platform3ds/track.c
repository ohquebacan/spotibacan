#include "track.h"
#include "http3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"

/* Every Spotify audio file uses this IV with the per-file AES-128 key, in
 * counter mode, with the counter running from the start of the file. */
static const uint8_t AUDIO_AESIV[16] = {
	0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
	0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
};

void track_free(track_audio *t)
{
	if (!t)
		return;
	free(t->data);
	t->data = NULL;
	t->len = 0;
}

static bool decrypt(uint8_t *buf, size_t len, const uint8_t key[16])
{
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0;
	if (ok) {
		uint8_t nonce[16];
		uint8_t stream_block[16];
		size_t nc_off = 0;
		memcpy(nonce, AUDIO_AESIV, sizeof(nonce));
		/* The whole file is one counter run starting at zero, so this single
		 * call is equivalent to decrypting it chunk by chunk in order. */
		ok = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce, stream_block,
		                           buf, buf) == 0;
	}
	mbedtls_aes_free(&aes);
	return ok;
}

int track_fetch(const char *sp_host, const char *token, ap_session *ap,
                const char *track_uri, size_t bytes, size_t limit,
                void (*progress)(const char *step),
                track_audio *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	TickCounter t;

	if (progress) progress("metadata");
	osTickCounterStart(&t);
	if (sp_track_metadata(sp_host, token, track_uri, &out->meta,
	                      err, errlen) != 0)
		return -1;
	if (!out->meta.has_gid) {
		snprintf(err, errlen, "metadata carried no track id");
		return -1;
	}
	const sp_file *file = sp_pick_file(&out->meta);
	if (!file) {
		snprintf(err, errlen, "no Vorbis file this console can play");
		return -1;
	}
	out->format = file->format;

	if (progress) progress("storage-resolve");
	sp_storage storage;
	if (sp_storage_resolve(sp_host, token, file->id, &storage,
	                       err, errlen) < 0)
		return -1;
	osTickCounterUpdate(&t);
	out->resolve_ms = osTickCounterRead(&t);

	/* The key must be asked for before the bytes are worth fetching: without
	 * it the download is just noise. */
	if (progress) progress("clave de audio");
	osTickCounterStart(&t);
	uint8_t key[16];
	int refused = 0;
	int rc = ap_audio_key(ap, file->id, out->meta.gid, key, &refused,
	                      err, errlen);
	osTickCounterUpdate(&t);
	out->key_ms = osTickCounterRead(&t);
	if (rc != 0)
		return rc;

	if (progress) progress("descarga del CDN");
	osTickCounterStart(&t);
	http_reply r;
	int got = -1;
	for (int i = 0; i < storage.count; i++) {
		got = bytes ? http_get_url_range(storage.urls[i], bytes, limit, &r)
		            : http_get_url(storage.urls[i], limit, &r);
		if (got == 0 && (r.status == 200 || r.status == 206))
			break;
		/* A signed URL can have expired or its edge can be down; the other
		 * two serve the same bytes. */
		if (got == 0)
			http_reply_free(&r);
		got = -1;
	}
	osTickCounterUpdate(&t);
	out->download_ms = osTickCounterRead(&t);
	if (got != 0) {
		snprintf(err, errlen, "no CDN url served the file");
		return -1;
	}
	if (r.len <= SPOTIFY_OGG_HEADER) {
		snprintf(err, errlen, "file is only %u bytes", (unsigned)r.len);
		http_reply_free(&r);
		return -1;
	}

	if (progress) progress("descifrado");
	osTickCounterStart(&t);
	if (!decrypt((uint8_t *)r.body, r.len, key)) {
		snprintf(err, errlen, "AES-CTR failed");
		http_reply_free(&r);
		return -1;
	}
	osTickCounterUpdate(&t);
	out->decrypt_ms = osTickCounterRead(&t);

	/* Decryption is in place, so the reply's buffer becomes the track's. */
	out->data = (uint8_t *)r.body;
	out->len = r.len;

	if (memcmp(out->data + SPOTIFY_OGG_HEADER, "OggS", 4) != 0) {
		snprintf(err, errlen,
		         "no Ogg stream after the %d byte header -- wrong key?",
		         SPOTIFY_OGG_HEADER);
		track_free(out);
		return -1;
	}
	return 0;
}

int track_stream_begin(const char *sp_host, const char *token, ap_session *ap,
                       const char *track_uri, void (*progress)(const char *),
                       track_stream *out, char *err, size_t errlen)
{
	/* The fields set by the caller survive the reset. */
	const bool want_hq = out->want_hq;
	struct http_session *sess = out->sess;
	memset(out, 0, sizeof(*out));
	out->want_hq = want_hq;
	out->sess = sess;
	out->dl_restart_at = -1;

	/* The same three steps as track_fetch, in the same order: the key must
	 * be asked for before the bytes are worth fetching. */
	if (progress) progress("metadata");
	if (sp_track_metadata_s(out->sess, sp_host, token, track_uri, &out->meta,
	                        err, errlen) != 0)
		return -1;
	if (!out->meta.has_gid) {
		snprintf(err, errlen, "metadata carried no track id");
		return -1;
	}
	const sp_file *file = sp_pick_file_q(&out->meta, out->want_hq);
	if (!file) {
		snprintf(err, errlen, "no Vorbis file this console can play");
		return -1;
	}
	out->format = file->format;

	if (progress) progress("storage-resolve");
	if (sp_storage_resolve_s(out->sess, sp_host, token, file->id,
	                         &out->storage, err, errlen) < 0)
		return -1;

	if (progress) progress("clave de audio");
	int refused = 0;
	return ap_audio_key(ap, file->id, out->meta.gid, out->key, &refused,
	                    err, errlen);
}

static bool stream_cb(void *ctx, const char *data, size_t n)
{
	track_stream *ts = ctx;
	if (ts->stop && *ts->stop)
		return false;
	if (ts->dl_restart_at >= 0)
		return false;   /* a seek is moving the download; abandon this read */
	if (!ts->data) {
		if (!ts->total || ts->total > TRACK_STREAM_MAX) {
			snprintf(ts->dl_err, sizeof(ts->dl_err),
			         "file size unknown or over the %u MB cap",
			         TRACK_STREAM_MAX / (1024 * 1024));
			ts->cb_fatal = true;
			return false;
		}
		ts->data = malloc(ts->total);
		if (!ts->data) {
			snprintf(ts->dl_err, sizeof(ts->dl_err), "out of memory");
			ts->cb_fatal = true;
			return false;
		}
		/* Zero-filled, so a decoder bug that overran the watermark would
		 * parse as silence instead of as random heap. */
		memset(ts->data, 0, ts->total);
		ts->len = ts->total;
	}
	if (ts->write_pos + n > ts->len) {
		if (ts->write_pos >= ts->len)
			return true;   /* padding past the end: tolerate */
		n = ts->len - ts->write_pos;
	}
	memcpy(ts->data + ts->write_pos, data, n);
	if (mbedtls_aes_crypt_ctr(ts->aes, n, &ts->nc_off, ts->nonce,
	                          ts->stream_block, ts->data + ts->write_pos,
	                          ts->data + ts->write_pos) != 0) {
		snprintf(ts->dl_err, sizeof(ts->dl_err), "AES-CTR failed");
		ts->cb_fatal = true;
		return false;
	}
	const size_t start = ts->write_pos;
	ts->write_pos += n;
	/* Bytes landing on the hole's low edge shrink it. */
	if (ts->has_hole && start <= ts->hole_lo && ts->write_pos > ts->hole_lo) {
		ts->hole_lo = ts->write_pos;
		__sync_synchronize();
		if (ts->hole_lo >= ts->hole_hi)
			ts->has_hole = false;
	}
	__sync_synchronize();
	if (ts->filling) {
		if (!ts->has_hole) {
			/* Closed: the tail beyond it is already here. */
			ts->filling = false;
			ts->write_pos = ts->len;
			return false;
		}
		return true;   /* the tail is the watermark until then */
	}
	ts->downloaded = ts->write_pos;
	return true;
}

/* Moves the download cursor to an absolute, 16-byte-aligned file offset.
 * CTR mode is a keystream: the counter for byte N is the IV plus N/16 as a
 * 128-bit big-endian integer, so any offset is reachable without decrypting
 * what came before. */
static void stream_keystream_at(track_stream *ts, size_t at)
{
	memcpy(ts->nonce, AUDIO_AESIV, sizeof(ts->nonce));
	uint64_t add = at / 16;
	for (int i = 15; i >= 0 && add; i--) {
		unsigned v = ts->nonce[i] + (unsigned)(add & 0xFF);
		ts->nonce[i] = (uint8_t)v;
		add = (add >> 8) + (v >> 8);
	}
	ts->nc_off = 0;
	ts->write_pos = at;
}

/* A seek moving the download. Jumping forward leaves [write_pos, at) as
 * zero fill: that is recorded as the hole (merged with any earlier one,
 * conservatively) so the decoder cannot run into it and the file is not
 * saved with it. Jumping into the hole leaves only [hole_lo, at) owing. */
static void stream_rewind(track_stream *ts, size_t at)
{
	at &= ~(size_t)15;
	if (ts->len && at >= ts->len)
		at = ts->len & ~(size_t)15;
	ts->filling = false;
	if (at > ts->write_pos) {
		size_t lo = ts->write_pos;
		if (ts->has_hole && ts->hole_lo < lo)
			lo = ts->hole_lo;
		ts->hole_lo = lo;
		ts->hole_hi = at;
		__sync_synchronize();
		ts->has_hole = true;
	} else if (ts->has_hole && at > ts->hole_lo && at < ts->hole_hi) {
		ts->hole_hi = at;
	}
	stream_keystream_at(ts, at);
	__sync_synchronize();
	ts->downloaded = at;
}

/* The tail reached the end with a hole still open: go back and fill it,
 * leaving the published watermark at the tail for the decoder. */
static void stream_fill_hole(track_stream *ts)
{
	ts->filling = true;
	stream_keystream_at(ts, ts->hole_lo & ~(size_t)15);
}

int track_stream_download(track_stream *ts, volatile bool *stop)
{
	ts->stop = stop;
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	if (mbedtls_aes_setkey_enc(&aes, ts->key, 128) != 0) {
		snprintf(ts->dl_err, sizeof(ts->dl_err), "AES-CTR setup failed");
		ts->dl_failed = true;
		mbedtls_aes_free(&aes);
		return -1;
	}
	memcpy(ts->nonce, AUDIO_AESIV, sizeof(ts->nonce));
	ts->nc_off = 0;
	ts->aes = &aes;

	/* Keep at it while the link comes and goes: walking through a dead spot
	 * used to burn the two passes in seconds and end the song mid-play.
	 * Every attempt resumes where the last one stopped; the clock only runs
	 * while nothing arrives, and any progress resets it. A reposition
	 * starts the clock afresh from the new offset. */
	u64 stalled_since = 0;
	unsigned backoff_ms = 500;
	while (!ts->dl_done && !ts->cb_fatal && !(stop && *stop)) {
		if (ts->dl_restart_at >= 0) {
			size_t at = (size_t)ts->dl_restart_at;
			ts->dl_restart_at = -1;
			stream_rewind(ts, at);
			stalled_since = 0;
			backoff_ms = 500;
		}
		const size_t before = ts->write_pos;
		for (int i = 0; i < ts->storage.count; i++) {
			if ((stop && *stop) || ts->cb_fatal || ts->dl_restart_at >= 0)
				break;
			int status = 0;
			ts->dl_err[0] = '\0';   /* this try's reason, not an old one */
			http_get_url_stream(ts->storage.urls[i], ts->write_pos,
			                    stream_cb, ts, &ts->total, &status,
			                    ts->dl_err, sizeof(ts->dl_err));
			if (ts->len && ts->write_pos >= ts->len) {
				if (!ts->has_hole) {
					ts->dl_done = true;
				} else if (!ts->filling) {
					stream_fill_hole(ts);
					i--;   /* same url, from the hole */
					continue;
				}
			}
			if (ts->dl_done || (stop && *stop))
				break;
		}
		if (ts->dl_done || ts->cb_fatal || (stop && *stop) ||
		    ts->dl_restart_at >= 0)
			continue;
		const u64 now = osGetTime();
		if (ts->write_pos > before) {
			stalled_since = 0;   /* it moved: the link is back */
			backoff_ms = 500;
		} else if (!stalled_since) {
			stalled_since = now;
		} else if (now - stalled_since > TRACK_STALL_GIVEUP_MS) {
			break;   /* a minute and a half of nothing: really gone */
		}
		/* Let the Wi-Fi reassociate before the next try, checking for a
		 * stop or a seek every 100 ms. */
		for (unsigned w = 0; w < backoff_ms && !(stop && *stop) &&
		                     ts->dl_restart_at < 0; w += 100)
			svcSleepThread(100000000LL);
		if (backoff_ms < 4000)
			backoff_ms *= 2;
	}
	ts->aes = NULL;
	mbedtls_aes_free(&aes);

	if (stop && *stop)
		return 0;   /* the UI moved on; not an error */
	if (!ts->dl_done) {
		ts->dl_failed = true;
		if (!ts->dl_err[0])
			snprintf(ts->dl_err, sizeof(ts->dl_err),
			         "no CDN url served the file");
		return -1;
	}
	return 0;
}

void track_stream_free(track_stream *ts)
{
	if (!ts)
		return;
	free(ts->data);
	ts->data = NULL;
	ts->len = ts->total = ts->write_pos = 0;
	ts->downloaded = 0;
}
