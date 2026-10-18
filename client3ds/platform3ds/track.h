/* One playable track: from a URI to decrypted Ogg Vorbis in memory.
 *
 * Ties together the three services that each hold one piece: spclient for the
 * metadata and the CDN location, the access point for the decryption key, and
 * the CDN for the bytes. None of them is useful without the other two.
 */
#ifndef TRACK_H
#define TRACK_H

#include "ap3ds.h"
#include "spclient.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Spotify puts a decoy Ogg page of this length at the start of every audio
 * file; the real Vorbis stream begins right after it.
 *
 * Do not try to find this by searching for the "OggS" magic: the decoy *is* an
 * OggS page, so the first match is the wrong one and the decoder then fails
 * with nothing more useful than "end of stream". */
#define SPOTIFY_OGG_HEADER 167

typedef struct {
	uint8_t *data;           /* decrypted file, including the decoy header */
	size_t   len;
	sp_track meta;
	int      format;         /* which AudioFile.Format was fetched */
	double   resolve_ms;
	double   key_ms;
	double   download_ms;
	double   decrypt_ms;
} track_audio;

/* Fetches and decrypts one track. `ap` must be a live session: the audio key
 * comes from there and nowhere else.
 *
 * `limit` caps the download; a track that would exceed it fails rather than
 * being truncated into something that decodes to silence halfway through.
 *
 * Returns 0, AP_KEY_REFUSED when Spotify withholds the key for this account,
 * or -1. */
/* `bytes` asks the CDN for only the first N bytes; 0 fetches the whole file.
 * `progress` may be NULL -- without it, four services hide behind one call. */
int  track_fetch(const char *sp_host, const char *token, ap_session *ap,
                 const char *track_uri, size_t bytes, size_t limit,
                 void (*progress)(const char *step),
                 track_audio *out, char *err, size_t errlen);

void track_free(track_audio *t);

/* A track being streamed rather than fetched whole.
 *
 * The file's full size is known from the first reply's headers, so the
 * buffer is reserved complete up front and a download thread fills it in
 * order while the decoder walks behind it. The two never race over content:
 * `downloaded` is the count of *decrypted* bytes, published only after each
 * piece lands, and the decoder must never read past it. */
#define TRACK_STREAM_MAX (12 * 1024 * 1024)

/* How long a download keeps retrying with nothing arriving before it gives
 * up on the track. Long enough to walk through a dead spot in the house. */
#define TRACK_STALL_GIVEUP_MS 90000u

typedef struct {
	/* Set by the caller before track_stream_begin: a download for the
	 * card may take the 320 kbps file, streaming always takes 160. */
	bool     want_hq;
	/* Optional keep-alive session to the spclient host for metadata and
	 * storage-resolve (caller-owned, one thread at a time). */
	struct http_session *sess;
	uint8_t *data;              /* reserved whole file; NULL until the
	                             * total is known */
	size_t   len;               /* the file's full size */
	size_t   total;             /* what the reply headers reported */
	size_t   write_pos;         /* download cursor, download thread only */
	volatile size_t downloaded; /* decrypted watermark, read by the decoder:
	                             * bytes available up to this absolute offset
	                             * in the current download span */
	volatile bool dl_done;      /* write_pos reached len */
	volatile bool dl_failed;    /* gave up; dl_err says why */
	bool     cb_fatal;          /* a real error, not worth retrying */
	char     dl_err[160];
	/* A seek past the watermark writes an absolute byte offset here; the
	 * download thread aborts the current request and resumes from there,
	 * leaving a zero hole behind (see has_hole). -1 = no reposition
	 * pending. */
	volatile long dl_restart_at;
	/* The span a forward seek skipped, [hole_lo, hole_hi): still zero fill
	 * while has_hole. The decoder must not walk into it, and the file is
	 * not done (nor saved) until a fill pass has closed it. hole_lo moves
	 * up as bytes land on it. Download thread writes, decoder reads. */
	volatile bool   has_hole;
	volatile size_t hole_lo, hole_hi;
	bool            filling;     /* closing the hole after the tail landed:
	                              * `downloaded` stays at the tail meanwhile */

	sp_track   meta;
	int        format;
	uint8_t    key[16];
	sp_storage storage;

	/* Running AES-CTR state, owned by track_stream_download for its
	 * lifetime: pieces arrive in file order, so one context decrypts them
	 * as they land and the watermark never exposes encrypted bytes. */
	void    *aes;
	uint8_t  nonce[16];
	uint8_t  stream_block[16];
	size_t   nc_off;
	volatile bool *stop;
} track_stream;

/* Metadata, storage-resolve and the audio key: everything that must happen
 * before the first byte is worth downloading. Same steps and order as
 * track_fetch. Returns 0, AP_KEY_REFUSED, or -1. */
int  track_stream_begin(const char *sp_host, const char *token, ap_session *ap,
                        const char *track_uri, void (*progress)(const char *),
                        track_stream *out, char *err, size_t errlen);

/* Fills the reserved buffer, retrying across the CDN URLs and resuming each
 * attempt where the last one stopped. Returns early and quietly when `stop`
 * goes up -- that is the UI skipping, not a failure. */
int  track_stream_download(track_stream *ts, volatile bool *stop);
void track_stream_free(track_stream *ts);

/* Where the Vorbis stream starts, and how long it is. */
static inline const uint8_t *track_vorbis(const track_audio *t)
{
	return t->len > SPOTIFY_OGG_HEADER ? t->data + SPOTIFY_OGG_HEADER : NULL;
}
static inline size_t track_vorbis_len(const track_audio *t)
{
	return t->len > SPOTIFY_OGG_HEADER ? t->len - SPOTIFY_OGG_HEADER : 0;
}

#endif
