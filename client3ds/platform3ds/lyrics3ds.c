#include "lyrics3ds.h"

#include "http3ds.h"
#include "json3ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parses one "[mm:ss.xx] text" line. Metadata tags ([ti:], [ar:] and
 * friends) and malformed lines are skipped; a line may carry several
 * timestamps, and only the first is used. */
static bool lrc_line(const char *p, size_t len, uint32_t *time_ms,
                     char *text, size_t textlen)
{
	if (len < 10 || p[0] != '[')
		return false;
	const char *close = memchr(p, ']', len);
	if (!close)
		return false;

	long mm = 0, ss = 0, frac = 0;
	int ndigits = 0;
	const char *q = p + 1;
	if (close - q < 5 || q[0] < '0' || q[0] > '9')
		return false;   /* a metadata tag, not a timestamp */
	while (q < close && *q >= '0' && *q <= '9')
		mm = mm * 10 + (*q++ - '0');
	if (q >= close || *q++ != ':')
		return false;
	if (close - q < 2)
		return false;
	ss = (*q++ - '0') * 10;
	ss += (*q++ - '0');
	if (q < close && (*q == '.' || *q == ',')) {
		q++;
		while (q < close && *q >= '0' && *q <= '9' && ndigits < 3) {
			frac = frac * 10 + (*q++ - '0');
			ndigits++;
		}
	}
	/* Two fraction digits are centiseconds, three are milliseconds. */
	while (ndigits++ < 3)
		frac *= 10;
	*time_ms = (uint32_t)(mm * 60000 + ss * 1000 + frac);

	q = close + 1;
	size_t n = (size_t)(p + len - q);
	while (n && (q[n - 1] == '\r' || q[n - 1] == ' '))
		n--;
	if (n > textlen - 1)
		n = textlen - 1;
	memcpy(text, q, n);
	text[n] = '\0';
	return true;
}

static void parse_lrc(const char *text, lyrics_doc *out)
{
	out->count = 0;
	out->synced = true;
	const char *p = text;
	while (*p && out->count < LYRICS_MAX_LINES) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		lyric_line *l = &out->lines[out->count];
		if (lrc_line(p, len, &l->time_ms, l->text, sizeof(l->text)))
			out->count++;
		if (!nl)
			break;
		p = nl + 1;
	}
	/* LRCLIB serves its lines sorted; a small insertion pass keeps that
	 * guarantee instead of assuming it. */
	for (int i = 1; i < out->count; i++) {
		lyric_line key = out->lines[i];
		int j = i - 1;
		while (j >= 0 && out->lines[j].time_ms > key.time_ms) {
			out->lines[j + 1] = out->lines[j];
			j--;
		}
		out->lines[j + 1] = key;
	}
}

static void parse_plain(const char *text, lyrics_doc *out)
{
	out->count = 0;
	out->synced = false;
	const char *p = text;
	while (*p && out->count < LYRICS_MAX_LINES) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len) {
			lyric_line *l = &out->lines[out->count++];
			l->time_ms = UINT32_MAX;
			if (len > LYRICS_TEXT_MAX)
				len = LYRICS_TEXT_MAX;
			memcpy(l->text, p, len);
			l->text[len] = '\0';
		}
		if (!nl)
			break;
		p = nl + 1;
	}
}

lyrics_result lyrics_fetch(const char *track, const char *artist,
                           const char *album, long duration_ms,
                           lyrics_doc *out, char *err, size_t errlen)
{
	if (!track || !*track || !artist) {
		snprintf(err, errlen, "no track");
		return LYR_ERR;
	}

	char etrack[400], eartist[400], ealbum[400];
	url_encode(track, etrack, sizeof(etrack));
	url_encode(artist, eartist, sizeof(eartist));
	if (album)
		url_encode(album, ealbum, sizeof(ealbum));
	else
		ealbum[0] = '\0';

	char path[1400];
	snprintf(path, sizeof(path),
	         "/api/get?artist_name=%s&track_name=%s&album_name=%s&duration=%ld",
	         eartist, etrack, ealbum, duration_ms / 1000);

	http_reply r;
	memset(&r, 0, sizeof(r));
	if (http_get("lrclib.net", path, NULL, &r) != 0) {
		snprintf(err, errlen, "lrclib: %s", r.err);
		return LYR_ERR;
	}
	if (r.status == 404) {
		http_reply_free(&r);
		return LYR_NONE;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "lrclib returned HTTP %d", r.status);
		http_reply_free(&r);
		return LYR_ERR;
	}

	long instrumental = 0;
	json_int(r.body, "instrumental", &instrumental);
	if (instrumental) {
		http_reply_free(&r);
		return LYR_NONE;
	}

	/* The lyrics themselves can be long; the parse buffers live in BSS
	 * rather than on a worker's stack. */
	static char lrc[48 * 1024];
	lrc[0] = '\0';
	json_str(r.body, "syncedLyrics", lrc, sizeof(lrc));
	if (lrc[0]) {
		parse_lrc(lrc, out);
		http_reply_free(&r);
		return out->count ? LYR_OK : LYR_NONE;
	}
	json_str(r.body, "plainLyrics", lrc, sizeof(lrc));
	http_reply_free(&r);
	if (lrc[0]) {
		parse_plain(lrc, out);
		return out->count ? LYR_OK : LYR_NONE;
	}
	return LYR_NONE;
}

int lyrics_index_at(const lyrics_doc *doc, uint32_t position_ms)
{
	if (!doc->synced)
		return -1;
	int found = -1;
	for (int i = 0; i < doc->count && doc->lines[i].time_ms <= position_ms;
	     i++)
		found = i;
	return found;
}
