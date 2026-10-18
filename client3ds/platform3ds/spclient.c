#include "spclient.h"
#include "http3ds.h"
#include "json3ds.h"
#include "proto3ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPCLIENT_FALLBACK "spclient.wg.spotify.com"

/* extended_metadata.proto: BatchedEntityRequest { header 1, entity_request 2 },
 * EntityRequest { entity_uri 1, query 2 }, ExtensionQuery { extension_kind 1 }.
 * Putting the entity request in field 1 yields HTTP 200 and an empty body,
 * which is a confusing way to be told the shape is wrong. */
#define EXT_KIND_ARTIST_V4 8
#define EXT_KIND_ALBUM_V4  9
#define EXT_KIND_TRACK_V4 10

/* Defined with the playlist code below; the rootlist is the same shape and
 * shares it. */
static int read_list_items(const pbv *content, char uris[][64], int max);

static void copy_str(char *dst, size_t cap, const pbv *v)
{
	size_t n = v->len < cap - 1 ? v->len : cap - 1;
	memcpy(dst, v->p, n);
	dst[n] = '\0';
}

/* Collects the image ids of a container holding repeated Image entries under
 * `field`, indexed by the Image.size enum: 0=DEFAULT (300px), 1=SMALL (64px),
 * 2=LARGE (640px). Image is { file_id 1, size 2 }; the id becomes the URL in
 * lowercase hex. The repeated entries are walked by hand for the same reason
 * the audio files are. */
static void read_image_ids(const pbv *container, uint32_t field, char ids[3][41])
{
	size_t at = 0;
	while (at < container->len) {
		pbv rest = { container->p + at, container->len - at };
		pbv img, fid;
		uint64_t size = 0;
		if (!pb_get_bytes(rest.p, rest.len, field, &img))
			break;
		pb_get_varint(img.p, img.len, 2, &size);
		if (pb_get_bytes(img.p, img.len, 1, &fid) && fid.len == 20 &&
		    size <= 2 && !ids[size][0]) {
			static const char HEX[] = "0123456789abcdef";
			for (int i = 0; i < 20; i++) {
				ids[size][i * 2] = HEX[fid.p[i] >> 4];
				ids[size][i * 2 + 1] = HEX[fid.p[i] & 15];
			}
		}
		at = (size_t)(img.p + img.len - container->p);
	}
}

/* The album's cover images as i.scdn.co URLs.
 *
 * Track.album (3) -> Album.cover_group (17) -> ImageGroup.image (1, repeated)
 * -> Image. Older metadata used Album.cover (9), a repeated Image directly on
 * the album, so that is the fallback when no ImageGroup is present. Reading
 * the wrong field number here fails silently: the protobuf scan just finds
 * nothing and every cover URL stays empty. Falls back across sizes, so a
 * missing SMALL does not cost the list its thumbnails. */
static void read_cover_urls(const pbv *track, sp_track *out)
{
	pbv album, group;
	if (!pb_get_bytes(track->p, track->len, 3, &album))
		return;

	char ids[3][41] = {{0}};
	if (pb_get_bytes(album.p, album.len, 17, &group))
		read_image_ids(&group, 1, ids);
	if (!ids[0][0] && !ids[1][0] && !ids[2][0])
		read_image_ids(&album, 9, ids);

	const char *small = ids[1][0] ? ids[1] : ids[2][0] ? ids[2] : ids[0];
	/* The "large" cover is drawn at 128px at most, so the 300px image
	 * (size 0) is plenty: the 640px one is ~4x the bytes to fetch over TLS
	 * and the pixels to decode, and cost 3-8 s per saved song. */
	const char *large = ids[0][0] ? ids[0] : ids[2][0] ? ids[2] : ids[1];
	if (small[0])
		snprintf(out->art_small, sizeof(out->art_small),
		         "https://i.scdn.co/image/%s", small);
	if (large[0])
		snprintf(out->art_large, sizeof(out->art_large),
		         "https://i.scdn.co/image/%s", large);
}

const char *sp_format_name(int format)
{
	switch (format) {
	case SP_FMT_OGG_96:  return "Vorbis 96";
	case SP_FMT_OGG_160: return "Vorbis 160";
	case SP_FMT_OGG_320: return "Vorbis 320";
	case 3: case 4: case 5: case 6: case 7: return "MP3";
	case 8: case 9: return "AAC";
	case 16: case 22: return "FLAC";
	default: return "other";
	}
}

static volatile bool s_high_quality;

void sp_set_high_quality(bool on) { s_high_quality = on; }
bool sp_high_quality(void) { return s_high_quality; }

const sp_file *sp_pick_file(const sp_track *t)
{
	return sp_pick_file_q(t, s_high_quality);
}

const sp_file *sp_pick_file_q(const sp_track *t, bool hq)
{
	/* 320 when asked for and the file fits the buffer: about 40 KB a
	 * second, plus headroom for Vorbis running over its nominal rate. */
	if (hq && t->duration_ms > 0 &&
	    (uint64_t)t->duration_ms * 40 * 11 / 10 + 65536 < SP_HQ_MAX_BYTES)
		for (int i = 0; i < t->file_count; i++)
			if (t->files[i].format == SP_FMT_OGG_320)
				return &t->files[i];
	/* 160 first, then 96, then 320: the order of what this console should
	 * actually stream and decode, not of audio quality. */
	static const int WANT[] = { SP_FMT_OGG_160, SP_FMT_OGG_96, SP_FMT_OGG_320 };
	for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++)
		for (int i = 0; i < t->file_count; i++)
			if (t->files[i].format == WANT[w])
				return &t->files[i];
	return NULL;
}

int sp_resolve(char *host, size_t hostlen, char *err, size_t errlen)
{
	http_reply r;
	if (http_get("apresolve.spotify.com", "/?type=spclient", NULL, &r) == 0 &&
	    r.status == 200) {
		const char *key = strstr(r.body, "\"spclient\"");
		const char *open = key ? strchr(key, '[') : NULL;
		const char *q1 = open ? strchr(open, '"') : NULL;
		const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (q2) {
			char entry[160];
			size_t n = (size_t)(q2 - q1 - 1);
			if (n < sizeof(entry)) {
				memcpy(entry, q1 + 1, n);
				entry[n] = '\0';
				/* The entry carries a port; 443 is implied and the port is
				 * dropped, since everything here goes over TLS. */
				char *colon = strrchr(entry, ':');
				if (colon)
					*colon = '\0';
				snprintf(host, hostlen, "%s", entry);
				http_reply_free(&r);
				return 0;
			}
		}
	}
	http_reply_free(&r);
	snprintf(host, hostlen, "%s", SPCLIENT_FALLBACK);
	snprintf(err, errlen, "apresolve gave no spclient; using %s", SPCLIENT_FALLBACK);
	return 0;
}

/* One request over the caller's keep-alive session when it has one, else
 * over a fresh connection. A fresh TLS handshake costs the ARM11 a second
 * or two; the player pays it for nothing on every track otherwise. */
static int sp_req(http_session *sess, const char *host, const char *path,
                  const char *method, const char *token, const char *ctype,
                  const void *body, size_t len, http_reply *r)
{
	if (sess)
		return http_session_request(sess, path, method, token, ctype,
		                            "application/x-protobuf", body, len, 0, r);
	return http_request(host, path, method, token, ctype,
	                    "application/x-protobuf", body, len, 0, r);
}

/* A 16-byte gid as the 22-character base62 id Spotify URIs carry: the gid
 * is one big-endian 128-bit number, written most significant digit first. */
static void gid_to_uri(const char *kind, const uint8_t *gid, char *out,
                       size_t outlen)
{
	static const char B62[] =
	    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	uint8_t num[16];
	memcpy(num, gid, 16);
	char digits[22];
	for (int d = 21; d >= 0; d--) {
		unsigned rem = 0;
		for (int i = 0; i < 16; i++) {   /* num /= 62, keep the remainder */
			unsigned cur = (rem << 8) | num[i];
			num[i] = (uint8_t)(cur / 62);
			rem = cur % 62;
		}
		digits[d] = B62[rem];
	}
	snprintf(out, outlen, "spotify:%s:%.22s", kind, digits);
}

int sp_track_metadata(const char *host, const char *token, const char *track_uri,
                      sp_track *out, char *err, size_t errlen)
{
	return sp_track_metadata_s(NULL, host, token, track_uri, out, err, errlen);
}

int sp_track_metadata_s(http_session *sess, const char *host, const char *token,
                        const char *track_uri, sp_track *out, char *err,
                        size_t errlen)
{
	memset(out, 0, sizeof(*out));
	snprintf(out->uri, sizeof(out->uri), "%s", track_uri);

	uint8_t q_store[16], er_store[256], req_store[320];
	pb q, er, req;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, EXT_KIND_TRACK_V4);
	pb_init(&er, er_store, sizeof(er_store));
	pb_str(&er, 1, track_uri);
	pb_sub(&er, 2, &q);
	pb_init(&req, req_store, sizeof(req_store));
	pb_sub(&req, 2, &er);
	if (!pb_ok(&req)) {
		snprintf(err, errlen, "metadata request did not fit");
		return -1;
	}

	http_reply r;
	/* The Bearer matters here as much as on the GETs: without it the service
	 * answers 200 with an empty body rather than 401, which reads like a
	 * malformed request. */
	if (sp_req(sess, host, "/extended-metadata/v0/extended-metadata", "POST",
	           token, "application/x-protobuf", req.p, req.len, &r) != 0) {
		snprintf(err, errlen, "metadata: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "metadata returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	pbv arr, ed, any, track;
	/* BatchedExtensionResponse.extended_metadata -> EntityExtensionDataArray
	 * .extension_data -> EntityExtensionData.extension_data (an Any) -> value */
	bool found = pb_get_bytes(body, r.len, 2, &arr) &&
	             pb_get_bytes(arr.p, arr.len, 3, &ed) &&
	             pb_get_bytes(ed.p, ed.len, 3, &any) &&
	             pb_get_bytes(any.p, any.len, 2, &track);
	if (!found) {
		snprintf(err, errlen, "no track in the metadata response (%u bytes)",
		         (unsigned)r.len);
		http_reply_free(&r);
		return -1;
	}

	pbv v;
	if (pb_get_bytes(track.p, track.len, 1, &v) && v.len == 16) {
		memcpy(out->gid, v.p, 16);
		out->has_gid = true;
	}
	if (pb_get_bytes(track.p, track.len, 2, &v))      /* Track.name */
		copy_str(out->name, sizeof(out->name), &v);
	int64_t dur = 0;
	/* Track.duration is a sint32: zigzag, not a plain varint. Read the wrong
	 * way it comes back doubled, which reads as a long-but-believable track
	 * rather than as an error. */
	if (pb_get_sint(track.p, track.len, 7, &dur))
		out->duration_ms = (int)dur;
	if (pb_get_bytes(track.p, track.len, 3, &v)) {    /* Track.album */
		pbv an;
		if (pb_get_bytes(v.p, v.len, 2, &an))         /* Album.name */
			copy_str(out->album, sizeof(out->album), &an);
		if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)   /* gid */
			gid_to_uri("album", an.p, out->album_uri, sizeof(out->album_uri));
	}
	read_cover_urls(&track, out);
	if (pb_get_bytes(track.p, track.len, 4, &v)) {    /* first Artist */
		pbv an;
		/* Artist.gid is field 1 and Artist.name is field 2; reading the first
		 * field here would hand back raw bytes that are not text. */
		if (pb_get_bytes(v.p, v.len, 2, &an))
			copy_str(out->artist, sizeof(out->artist), &an);
		if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
			gid_to_uri("artist", an.p, out->artist_uri,
			           sizeof(out->artist_uri));
	}

	/* Track.file is repeated, and pb_get_bytes only yields the first, so the
	 * files are walked by hand. */
	size_t at = 0;
	while (at < track.len && out->file_count < SP_MAX_FILES) {
		pbv rest = { track.p + at, track.len - at };
		pbv file;
		if (!pb_get_bytes(rest.p, rest.len, 12, &file))
			break;
		pbv fid;
		uint64_t fmt = 0;
		if (pb_get_bytes(file.p, file.len, 1, &fid) && fid.len == 20) {
			memcpy(out->files[out->file_count].id, fid.p, 20);
			pb_get_varint(file.p, file.len, 2, &fmt);
			out->files[out->file_count].format = (int)fmt;
			out->file_count++;
		}
		at = (size_t)(file.p + file.len - track.p);
	}

	http_reply_free(&r);
	return 0;
}

int sp_rootlist(const char *host, const char *token, const char *username,
                char uris[][64], char names[][64], char arts[][80], int max,
                int *count, char *err, size_t errlen)
{
	char path[320];
	snprintf(path, sizeof(path),
	         "/playlist/v2/user/%s/rootlist?decorate=revision,attributes,length",
	         username);

	http_reply r;
	if (http_get_proto(host, path, token, &r) != 0) {
		snprintf(err, errlen, "rootlist: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "rootlist returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	uint64_t length = 0;
	pb_get_varint(body, r.len, 2, &length);   /* SelectedListContent.length */
	*count = (int)length;

	pbv whole = { body, r.len };
	int n = read_list_items(&whole, uris, max);

	/* meta_items runs parallel to items, one per entry, and carries the
	 * ListAttributes the decoration asked for -- which is how the rows get
	 * real names without one request each. The same attributes carry the
	 * cover: `picture` (3) is the 20-byte image id for user-set covers,
	 * while generated ones (mosaic/pickasso) arrive as a format attribute
	 * with key "image_url". */
	if (names) {
		for (int i = 0; i < n; i++) {
			names[i][0] = '\0';
			if (arts)
				arts[i][0] = '\0';
		}
		pbv contents;
		if (pb_get_bytes(body, r.len, 5, &contents)) {
			size_t at = 0;
			int i = 0;
			while (at < contents.len && i < n) {
				pbv rest = { contents.p + at, contents.len - at };
				pbv meta;
				if (!pb_get_bytes(rest.p, rest.len, 4, &meta))
					break;
				pbv attrs, nm, pic;
				if (pb_get_bytes(meta.p, meta.len, 2, &attrs)) {
					if (pb_get_bytes(attrs.p, attrs.len, 1, &nm)) {
						size_t len = nm.len < 63 ? nm.len : 63;
						memcpy(names[i], nm.p, len);
						names[i][len] = '\0';
					}
					if (arts &&
					    pb_get_bytes(attrs.p, attrs.len, 3, &pic)) {
						if (pic.len == 20) {
							/* i.scdn.co/image/<hex id> is the CDN shape
							 * every 64px thumb URL already uses. */
							static const char hexd[] = "0123456789abcdef";
							snprintf(arts[i], 80,
							         "https://i.scdn.co/image/");
							char *o = arts[i] + strlen(arts[i]);
							for (int k = 0; k < 20; k++) {
								*o++ = hexd[pic.p[k] >> 4];
								*o++ = hexd[pic.p[k] & 15];
							}
							*o = '\0';
						} else if (pic.len > 8 && pic.len < 79 &&
						           memcmp(pic.p, "https://", 8) == 0) {
							memcpy(arts[i], pic.p, pic.len);
							arts[i][pic.len] = '\0';
						}
					}
				}
				i++;
				at = (size_t)(meta.p + meta.len - contents.p);
			}
		}
	}

	http_reply_free(&r);
	return n;
}

/* ------------------------------------------------------------- playlists */

/* SelectedListContent.contents (5) -> ListItems.items (3) -> Item.uri (1).
 * `items` is repeated, and pb_get_bytes only yields the first, so the list is
 * walked by hand. */
static int read_list_items(const pbv *content, char uris[][64], int max)
{
	pbv contents;
	if (!pb_get_bytes(content->p, content->len, 5, &contents))
		return 0;

	int n = 0;
	size_t at = 0;
	while (at < contents.len && n < max) {
		pbv rest = { contents.p + at, contents.len - at };
		pbv item;
		if (!pb_get_bytes(rest.p, rest.len, 3, &item))
			break;
		pbv uri;
		if (pb_get_bytes(item.p, item.len, 1, &uri)) {
			size_t len = uri.len < 63 ? uri.len : 63;
			memcpy(uris[n], uri.p, len);
			uris[n][len] = '\0';
			n++;
		}
		at = (size_t)(item.p + item.len - contents.p);
	}
	return n;
}

int sp_playlist(const char *host, const char *token, const char *playlist_id,
                char *name, size_t namelen, char uris[][64], int max,
                int *count, char *err, size_t errlen)
{
	if (name && namelen)
		name[0] = '\0';

	char path[160];
	snprintf(path, sizeof(path), "/playlist/v2/playlist/%s", playlist_id);

	http_reply r;
	if (http_get_proto(host, path, token, &r) != 0) {
		snprintf(err, errlen, "playlist: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "playlist returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	uint64_t length = 0;
	pb_get_varint(body, r.len, 2, &length);
	if (count)
		*count = (int)length;

	pbv attrs, nm;
	if (name && namelen && pb_get_bytes(body, r.len, 3, &attrs) &&
	    pb_get_bytes(attrs.p, attrs.len, 1, &nm))      /* ListAttributes.name */
		copy_str(name, namelen, &nm);

	pbv whole = { body, r.len };
	int n = read_list_items(&whole, uris, max);
	http_reply_free(&r);

	/* The service may hand back only the head of a long list (length says
	 * how many there are): ask for the rest a window at a time. A reply
	 * whose ListItems.pos is not where we asked means `from` was ignored,
	 * and its rows would only repeat the head. */
	int pages = 0;
	while (n < (int)length && n < max && pages++ < 20) {
		int want = max - n < 100 ? max - n : 100;
		snprintf(path, sizeof(path),
		         "/playlist/v2/playlist/%s?from=%d&length=%d",
		         playlist_id, n, want);
		if (http_get_proto(host, path, token, &r) != 0)
			break;
		int got = 0;
		pbv contents;
		uint64_t pos = 0;
		if (r.status == 200 &&
		    pb_get_bytes((const uint8_t *)r.body, r.len, 5, &contents) &&
		    pb_get_varint(contents.p, contents.len, 1, &pos) &&
		    pos == (uint64_t)n) {
			pbv page = { (const uint8_t *)r.body, r.len };
			got = read_list_items(&page, uris + n, max - n);
		}
		http_reply_free(&r);
		if (got <= 0)
			break;
		n += got;
	}
	return n;
}

/* ------------------------------------------------------- context-resolve */

/* The uri out of one tracks[] entry. Liked entries carry a metadata object
 * that grows with the album title and can pass any fixed buffer, so the
 * entry is not copied whole: "uri" comes first, and a prefix finds it. A
 * whole-entry copy that skipped big entries is what dropped liked songs. */
static bool entry_uri(const char *obj, size_t len, char out[64])
{
	char buf[1024];
	size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
	memcpy(buf, obj, n);
	buf[n] = '\0';
	return json_str(buf, "uri", out, 64);
}

/* Appends the uris of one page's "tracks" array after the n already held. */
static int page_tracks(const char *page, char uris[][64], int n, int max,
                       bool tracks_only)
{
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (n < max && json_array_next(page, "tracks", &cursor, &obj, &objlen)) {
		char u[64];
		if (!entry_uri(obj, objlen, u))
			continue;
		if (tracks_only && strncmp(u, "spotify:track:", 14) != 0)
			continue;
		snprintf(uris[n++], 64, "%s", u);
	}
	return n;
}

/* Follows a page link: hm://<path> is served by spclient at /<path>. The
 * reply is one page; its own next_page_url, if any, goes back in `link`. */
static int fetch_page(const char *host, const char *token, char *link,
                      size_t linklen, char uris[][64], int n, int max,
                      bool tracks_only)
{
	const char *path = strncmp(link, "hm://", 5) == 0 ? link + 4 : link;
	if (*path != '/') {
		link[0] = '\0';
		return n;
	}
	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		link[0] = '\0';
		return n;
	}
	if (r.status == 200) {
		n = page_tracks(r.body, uris, n, max, tracks_only);
		if (!json_str(r.body, "next_page_url", link, linklen))
			link[0] = '\0';
	} else {
		link[0] = '\0';
	}
	http_reply_free(&r);
	return n;
}

/* context-resolve answers {"pages":[...]}: the first pages inline, later
 * ones often only as a "page_url" (a long liked list, an album's second
 * disc), and any page may end in a "next_page_url". Reading just the first
 * "tracks" array is what cut those lists short; this walks every page and
 * fetches the linked ones. */
static int context_pages(const char *host, const char *token,
                         const char *body, char uris[][64], int max,
                         bool tracks_only)
{
	int n = 0;
	int hops = 0;
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (n < max && json_array_next(body, "pages", &cursor, &obj, &objlen)) {
		char *page = malloc(objlen + 1);
		if (!page)
			break;
		memcpy(page, obj, objlen);
		page[objlen] = '\0';
		char link[256] = "";
		if (strstr(page, "\"tracks\""))
			n = page_tracks(page, uris, n, max, tracks_only);
		else
			json_str(page, "page_url", link, sizeof(link));
		if (!link[0])
			json_str(page, "next_page_url", link, sizeof(link));
		free(page);
		while (link[0] && n < max && hops++ < 40)
			n = fetch_page(host, token, link, sizeof(link), uris, n, max,
			               tracks_only);
	}
	return n;
}

int sp_liked(const char *host, const char *token, const char *username,
             char uris[][64], int max, char *err, size_t errlen)
{
	char path[192];
	snprintf(path, sizeof(path),
	         "/context-resolve/v1/spotify:user:%s:collection", username);

	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		snprintf(err, errlen, "liked: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "liked returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	/* {"pages":[{"tracks":[{"uri":"spotify:track:...", "metadata":{...}}, ...]}]} */
	int n = context_pages(host, token, r.body, uris, max, false);
	http_reply_free(&r);
	return n;
}

/* ------------------------------------------------------- search and radio */

/* json_str and json_int want a NUL-terminated string, and the array iterator
 * hands back a slice, so each object is copied out. They are small: the
 * biggest seen is a radio entry at a few hundred bytes. */
static bool slice_to_buf(const char *p, size_t len, char *buf, size_t cap)
{
	if (len >= cap)
		return false;
	memcpy(buf, p, len);
	buf[len] = '\0';
	return true;
}

static int metadata_batch_request(const char *host, http_session *sess,
                                  const char *token,
                                  const char *const *uris, int n,
                                  sp_track *out, char *err, size_t errlen)
{
	if (n <= 0)
		return 0;

	uint8_t q_store[16];
	pb q;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, EXT_KIND_TRACK_V4);

	size_t cap = (size_t)n * 128 + 64;
	uint8_t *req_store = malloc(cap);
	if (!req_store) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	pb req;
	pb_init(&req, req_store, cap);
	for (int i = 0; i < n; i++) {
		uint8_t er_store[192];
		pb er;
		pb_init(&er, er_store, sizeof(er_store));
		pb_str(&er, 1, uris[i]);
		pb_sub(&er, 2, &q);
		pb_sub(&req, 2, &er);          /* repeated entity_request */
	}
	if (!pb_ok(&req)) {
		snprintf(err, errlen, "metadata batch of %d did not fit", n);
		free(req_store);
		return -1;
	}

	http_reply r;
	int rc = sess
	    ? http_session_request(sess, "/extended-metadata/v0/extended-metadata",
	                           "POST", token, "application/x-protobuf",
	                           "application/x-protobuf", req.p, req.len, 0, &r)
	    : http_request(host, "/extended-metadata/v0/extended-metadata", "POST",
	                   token, "application/x-protobuf", "application/x-protobuf",
	                   req.p, req.len, 0, &r);
	free(req_store);
	if (rc != 0) {
		snprintf(err, errlen, "metadata batch: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "metadata batch returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	/* One EntityExtensionDataArray comes back per extension kind, holding one
	 * EntityExtensionData per track, in the order asked. */
	const uint8_t *body = (const uint8_t *)r.body;
	int filled = 0;
	pbv arr;
	if (pb_get_bytes(body, r.len, 2, &arr)) {
		size_t at = 0;
		while (at < arr.len && filled < n) {
			pbv rest = { arr.p + at, arr.len - at };
			pbv ed;
			if (!pb_get_bytes(rest.p, rest.len, 3, &ed))
				break;
			pbv uri, any, track;
			memset(&out[filled], 0, sizeof(out[filled]));
			if (pb_get_bytes(ed.p, ed.len, 2, &uri))
				copy_str(out[filled].uri, sizeof(out[filled].uri), &uri);
			if (pb_get_bytes(ed.p, ed.len, 3, &any) &&
			    pb_get_bytes(any.p, any.len, 2, &track)) {
				pbv v;
				if (pb_get_bytes(track.p, track.len, 1, &v) && v.len == 16) {
					memcpy(out[filled].gid, v.p, 16);
					out[filled].has_gid = true;
				}
				if (pb_get_bytes(track.p, track.len, 2, &v))
					copy_str(out[filled].name, sizeof(out[filled].name), &v);
				int64_t dur = 0;
				if (pb_get_sint(track.p, track.len, 7, &dur))
					out[filled].duration_ms = (int)dur;
				if (pb_get_bytes(track.p, track.len, 3, &v)) {
					pbv an;
					if (pb_get_bytes(v.p, v.len, 2, &an))
						copy_str(out[filled].album, sizeof(out[filled].album), &an);
					if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
						gid_to_uri("album", an.p, out[filled].album_uri,
						           sizeof(out[filled].album_uri));
				}
				read_cover_urls(&track, &out[filled]);
				if (pb_get_bytes(track.p, track.len, 4, &v)) {
					pbv an;
					if (pb_get_bytes(v.p, v.len, 2, &an))
						copy_str(out[filled].artist, sizeof(out[filled].artist), &an);
					if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
						gid_to_uri("artist", an.p, out[filled].artist_uri,
						           sizeof(out[filled].artist_uri));
				}
				size_t fat = 0;
				while (fat < track.len && out[filled].file_count < SP_MAX_FILES) {
					pbv frest = { track.p + fat, track.len - fat };
					pbv file;
					if (!pb_get_bytes(frest.p, frest.len, 12, &file))
						break;
					pbv fid;
					uint64_t fmt = 0;
					if (pb_get_bytes(file.p, file.len, 1, &fid) && fid.len == 20) {
						memcpy(out[filled].files[out[filled].file_count].id, fid.p, 20);
						pb_get_varint(file.p, file.len, 2, &fmt);
						out[filled].files[out[filled].file_count].format = (int)fmt;
						out[filled].file_count++;
					}
					fat = (size_t)(file.p + file.len - track.p);
				}
			}
			filled++;
			at = (size_t)(ed.p + ed.len - arr.p);
		}
	}

	http_reply_free(&r);
	if (filled == 0)
		snprintf(err, errlen, "no tracks in a %u byte response", (unsigned)r.len);
	return filled;
}

int sp_track_metadata_batch(const char *host, const char *token,
                            const char *const *uris, int n,
                            sp_track *out, char *err, size_t errlen)
{
	return metadata_batch_request(host, NULL, token, uris, n, out, err, errlen);
}

int sp_track_metadata_batch_s(http_session *sess, const char *token,
                              const char *const *uris, int n,
                              sp_track *out, char *err, size_t errlen)
{
	return metadata_batch_request(NULL, sess, token, uris, n, out, err, errlen);
}

int sp_search_tracks(const char *host, const char *token, const char *query,
                     char uris[][64], int max, char *err, size_t errlen)
{
	/* The context service takes the query inside the uri itself, with spaces
	 * as '+'. Everything else that is not plain ASCII alphanumerics is
	 * percent-encoded: this travels in a request line, and "Snødronningen"
	 * or "Bæ bæ" sent raw is UTF-8 the edge may refuse. A literal ':' or
	 * '+' in the query must not read as uri structure either. */
	static const char hexd[] = "0123456789ABCDEF";
	char plus[192];
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)query;
	     *p && n + 4 < sizeof(plus); p++) {
		if (*p == ' ')
			plus[n++] = '+';
		else if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
		         (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_' ||
		         *p == '.' || *p == '~')
			plus[n++] = (char)*p;
		else {
			plus[n++] = '%';
			plus[n++] = hexd[*p >> 4];
			plus[n++] = hexd[*p & 15];
		}
	}
	plus[n] = '\0';

	char path[320];
	snprintf(path, sizeof(path), "/context-resolve/v1/spotify:search:%s", plus);

	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		snprintf(err, errlen, "search: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "search returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	int count = 0;
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (count < max && json_array_next(r.body, "tracks", &cursor, &obj, &objlen)) {
		char buf[256];
		if (!slice_to_buf(obj, objlen, buf, sizeof(buf)))
			continue;
		char uri[64];
		if (json_str(buf, "uri", uri, sizeof(uri)))
			snprintf(uris[count++], 64, "%s", uri);
	}
	http_reply_free(&r);
	if (count == 0)
		snprintf(err, errlen, "no tracks matched");
	return count;
}

/* One extended-metadata round trip for n entities of one extension kind;
 * the caller parses r (and frees it) on success. */
static int ext_request(http_session *sess, const char *host, const char *token,
                       int kind, const char *const *uris, int n,
                       http_reply *r, char *err, size_t errlen)
{
	uint8_t q_store[16];
	pb q;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, (uint64_t)kind);
	size_t cap = (size_t)n * 128 + 64;
	uint8_t *req_store = malloc(cap);
	if (!req_store) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	pb req;
	pb_init(&req, req_store, cap);
	for (int i = 0; i < n; i++) {
		uint8_t er_store[192];
		pb er;
		pb_init(&er, er_store, sizeof(er_store));
		pb_str(&er, 1, uris[i]);
		pb_sub(&er, 2, &q);
		pb_sub(&req, 2, &er);
	}
	if (!pb_ok(&req)) {
		free(req_store);
		snprintf(err, errlen, "metadata request did not fit");
		return -1;
	}
	int rc = sp_req(sess, host, "/extended-metadata/v0/extended-metadata",
	                "POST", token, "application/x-protobuf", req.p, req.len, r);
	free(req_store);
	if (rc != 0) {
		snprintf(err, errlen, "metadata: %s", r->err);
		return -1;
	}
	if (r->status != 200) {
		snprintf(err, errlen, "metadata returned HTTP %d", r->status);
		http_reply_free(r);
		return -1;
	}
	return 0;
}

/* The message inside the i-th EntityExtensionData, by walking the array. */
static bool ext_entity(const http_reply *r, int want, pbv *uri, pbv *msg)
{
	pbv arr;
	if (!pb_get_bytes((const uint8_t *)r->body, r->len, 2, &arr))
		return false;
	size_t at = 0;
	for (int i = 0; at < arr.len; i++) {
		pbv rest = { arr.p + at, arr.len - at };
		pbv ed, any;
		if (!pb_get_bytes(rest.p, rest.len, 3, &ed))
			return false;
		at = (size_t)(ed.p + ed.len - arr.p);
		if (i != want)
			continue;
		if (!pb_get_bytes(ed.p, ed.len, 2, uri))
			uri->len = 0;
		return pb_get_bytes(ed.p, ed.len, 3, &any) &&
		       pb_get_bytes(any.p, any.len, 2, msg);
	}
	return false;
}

int sp_artist_releases(http_session *sess, const char *host, const char *token,
                       const char *artist_uri, sp_release *out, int max,
                       char *err, size_t errlen)
{
	http_reply r;
	const char *one[1] = { artist_uri };
	if (ext_request(sess, host, token, EXT_KIND_ARTIST_V4, one, 1, &r,
	                err, errlen) != 0)
		return -1;
	pbv uri, artist;
	if (!ext_entity(&r, 0, &uri, &artist)) {
		http_reply_free(&r);
		snprintf(err, errlen, "no artist metadata");
		return -1;
	}
	/* Artist.album_group (5) then single_group (6): each group lists the
	 * same release once per market, the first one is the one to use. */
	int n = 0;
	static const int GROUPS[2] = { 5, 6 };
	for (int g = 0; g < 2 && n < max; g++) {
		size_t at = 0;
		while (at < artist.len && n < max) {
			pbv rest = { artist.p + at, artist.len - at };
			pbv grp, alb, gid;
			if (!pb_get_bytes(rest.p, rest.len, (uint32_t)GROUPS[g], &grp))
				break;
			at = (size_t)(grp.p + grp.len - artist.p);
			if (!pb_get_bytes(grp.p, grp.len, 1, &alb) ||
			    !pb_get_bytes(alb.p, alb.len, 1, &gid) || gid.len != 16)
				continue;
			memset(&out[n], 0, sizeof(out[n]));
			gid_to_uri("album", gid.p, out[n].uri, sizeof(out[n].uri));
			out[n].type = g + 1;
			n++;
		}
	}
	http_reply_free(&r);
	if (n == 0)
		return 0;

	/* Names, years and covers: the artist record only carries the ids. */
	for (int base = 0; base < n; base += 50) {
		const int chunk = n - base > 50 ? 50 : n - base;
		const char *uris[50];
		for (int i = 0; i < chunk; i++)
			uris[i] = out[base + i].uri;
		if (ext_request(sess, host, token, EXT_KIND_ALBUM_V4, uris, chunk,
		                &r, err, errlen) != 0)
			break;   /* rows keep their ids; the view names them "Album" */
		/* Matched by the uri each answer carries, not by position: the
		 * service does not promise to answer in the order asked, and
		 * trusting it put one album's name on another's row. */
		for (int i = 0; i < chunk; i++) {
			pbv au, album, v;
			if (!ext_entity(&r, i, &au, &album))
				break;
			char got[48];
			copy_str(got, sizeof(got), &au);
			sp_release *rel = NULL;
			for (int j = 0; j < chunk; j++)
				if (strcmp(out[base + j].uri, got) == 0)
					rel = &out[base + j];
			if (!rel)
				continue;
			if (pb_get_bytes(album.p, album.len, 2, &v))
				copy_str(rel->name, sizeof(rel->name), &v);
			if (pb_get_bytes(album.p, album.len, 6, &v)) {   /* Date */
				uint64_t y = 0;
				if (pb_get_varint(v.p, v.len, 1, &y))
					rel->year = (int)y;
			}
			char ids[3][41] = {{0}};
			pbv group;
			if (pb_get_bytes(album.p, album.len, 17, &group))
				read_image_ids(&group, 1, ids);
			if (!ids[0][0] && !ids[1][0] && !ids[2][0])
				read_image_ids(&album, 9, ids);
			const char *small = ids[1][0] ? ids[1] : ids[0][0] ? ids[0] : ids[2];
			if (small[0])
				snprintf(rel->art, sizeof(rel->art),
				         "https://i.scdn.co/image/%s", small);
		}
		http_reply_free(&r);
	}
	return n;
}

int sp_playlist_add(const char *host, const char *token,
                    const char *playlist_uri, const char *track_uri,
                    char *err, size_t errlen)
{
	if (strncmp(playlist_uri, "spotify:playlist:", 17) != 0) {
		snprintf(err, errlen, "not a playlist");
		return -1;
	}
	const char *id = playlist_uri + 17;

	/* The current revision (and whether this account may edit it): a
	 * change is applied on top of a revision, never blind. */
	char path[160];
	snprintf(path, sizeof(path), "/playlist/v2/playlist/%s", id);
	http_reply r;
	if (http_get_proto(host, path, token, &r) != 0) {
		snprintf(err, errlen, "playlist: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "playlist returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}
	const uint8_t *body = (const uint8_t *)r.body;
	pbv rev, caps;
	uint8_t revision[64];
	size_t rev_len = 0;
	if (pb_get_bytes(body, r.len, 1, &rev) && rev.len <= sizeof(revision)) {
		memcpy(revision, rev.p, rev.len);
		rev_len = rev.len;
	}
	/* SelectedListContent.capabilities.can_edit_items, when sent. */
	bool editable = true;
	if (pb_get_bytes(body, r.len, 18, &caps)) {
		uint64_t can_edit = 0;
		editable = pb_get_varint(caps.p, caps.len, 5, &can_edit) && can_edit;
	}
	http_reply_free(&r);
	if (!editable) {
		snprintf(err, errlen, "not your playlist");
		return -1;
	}
	if (!rev_len) {
		snprintf(err, errlen, "playlist has no revision");
		return -1;
	}

	/* ListChanges{ base_revision, deltas:[ Delta{ ops:[ Op{ kind: ADD,
	 * add: Add{ items:[Item{uri}], add_last: true } } ] } ] } */
	uint8_t item_s[96], add_s[128], op_s[160], delta_s[192], lc_s[288];
	pb item, add, op, delta, lc;
	pb_init(&item, item_s, sizeof(item_s));
	pb_str(&item, 1, track_uri);
	pb_init(&add, add_s, sizeof(add_s));
	pb_sub(&add, 2, &item);
	pb_varint(&add, 4, 1);
	pb_init(&op, op_s, sizeof(op_s));
	pb_varint(&op, 1, 2);            /* Op.Kind.ADD */
	pb_sub(&op, 2, &add);
	pb_init(&delta, delta_s, sizeof(delta_s));
	pb_sub(&delta, 2, &op);
	pb_init(&lc, lc_s, sizeof(lc_s));
	pb_bytes(&lc, 1, revision, rev_len);
	pb_sub(&lc, 2, &delta);
	if (!pb_ok(&lc)) {
		snprintf(err, errlen, "change did not fit");
		return -1;
	}

	snprintf(path, sizeof(path), "/playlist/v2/playlist/%s/changes", id);
	if (http_request(host, path, "POST", token, "application/x-protobuf",
	                 "application/x-protobuf", lc.p, lc.len, 0, &r) != 0) {
		snprintf(err, errlen, "changes: %s", r.err);
		return -1;
	}
	const int status = r.status;
	http_reply_free(&r);
	if (status != 200) {
		if (status == 403)
			snprintf(err, errlen, "not your playlist");
		else
			snprintf(err, errlen, "changes returned HTTP %d", status);
		return -1;
	}
	return 0;
}

int sp_context_tracks(const char *host, const char *token, const char *uri,
                      char uris[][64], int max, char *err, size_t errlen)
{
	char path[160];
	snprintf(path, sizeof(path), "/context-resolve/v1/%s", uri);

	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		snprintf(err, errlen, "context: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "context returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	/* Same shape as liked: pages[].tracks[].uri, one page per disc. */
	int count = context_pages(host, token, r.body, uris, max, true);
	http_reply_free(&r);
	if (count == 0)
		snprintf(err, errlen, "no tracks in %s", uri);
	return count;
}

int sp_radio(const char *host, const char *token, const char *seed_uri,
             int count, sp_track *out, int max, char *err, size_t errlen)
{
	char path[256];
	snprintf(path, sizeof(path),
	         "/radio-apollo/v3/tracks/%s?autoplay=true&count=%d", seed_uri, count);

	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		snprintf(err, errlen, "radio: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "radio returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	int n = 0;
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (n < max && json_array_next(r.body, "tracks", &cursor, &obj, &objlen)) {
		char buf[1024];
		if (!slice_to_buf(obj, objlen, buf, sizeof(buf)))
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		json_str(buf, "uri", out[n].uri, sizeof(out[n].uri));
		/* These live in the entry's nested "metadata" object; json_str looks
		 * at any depth, so one pass over the entry finds them. */
		json_str(buf, "title", out[n].name, sizeof(out[n].name));
		json_str(buf, "artist_name", out[n].artist, sizeof(out[n].artist));
		json_str(buf, "album_title", out[n].album, sizeof(out[n].album));
		if (out[n].uri[0])
			n++;
	}
	http_reply_free(&r);
	if (n == 0)
		snprintf(err, errlen, "radio returned no tracks");
	return n;
}

/* --------------------------------------------------------- storage resolve */

int sp_storage_resolve(const char *host, const char *token,
                       const uint8_t file_id[20], sp_storage *out,
                       char *err, size_t errlen)
{
	return sp_storage_resolve_s(NULL, host, token, file_id, out, err, errlen);
}

int sp_storage_resolve_s(http_session *sess, const char *host,
                         const char *token, const uint8_t file_id[20],
                         sp_storage *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));

	char hex[41];
	for (int i = 0; i < 20; i++)
		snprintf(hex + i * 2, 3, "%02x", file_id[i]);

	char path[128];
	snprintf(path, sizeof(path),
	         "/storage-resolve/files/audio/interactive/%s", hex);

	http_reply r;
	if (sp_req(sess, host, path, "GET", token, NULL, NULL, 0, &r) != 0) {
		snprintf(err, errlen, "storage-resolve: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "storage-resolve returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	uint64_t result = 0;
	pb_get_varint(body, r.len, 1, &result);
	out->result = (int)result;

	/* cdnurl is repeated, so the list is walked by hand. */
	size_t at = 0;
	while (at < r.len && out->count < SP_MAX_CDN) {
		pbv rest = { body + at, r.len - at };
		pbv url;
		if (!pb_get_bytes(rest.p, rest.len, 2, &url))
			break;
		if (url.len < sizeof(out->urls[0])) {
			memcpy(out->urls[out->count], url.p, url.len);
			out->urls[out->count][url.len] = '\0';
			out->count++;
		}
		at = (size_t)(url.p + url.len - body);
	}
	http_reply_free(&r);

	if (out->count == 0) {
		snprintf(err, errlen, "no CDN url (result %d)", out->result);
		return -1;
	}
	return out->count;
}

int sp_user_image(const char *token, char *url, size_t urllen,
                  char *err, size_t errlen, int *retry_after)
{
	http_reply r;
	memset(&r, 0, sizeof(r));
	if (retry_after)
		*retry_after = 0;
	if (http_get("api.spotify.com", "/v1/me", token, &r) != 0) {
		snprintf(err, errlen, "profile: %s", r.err);
		http_reply_free(&r);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "profile: HTTP %d", r.status);
		if (r.status == 429 && retry_after)
			*retry_after = r.retry_after;
		http_reply_free(&r);
		return -1;
	}

	/* images is an array of {height, width, url}, largest first; the hero
	 * art is 128px, so any of them downscales fine. */
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	url[0] = '\0';
	while (json_array_next(r.body, "images", &cursor, &obj, &objlen)) {
		char buf[512], cand[96];
		if (objlen >= sizeof(buf))
			continue;
		memcpy(buf, obj, objlen);
		buf[objlen] = '\0';
		if (!json_str(buf, "url", cand, sizeof(cand)))
			continue;
		if (strncmp(cand, "https://i.scdn.co/image/", 24) != 0 ||
		    strlen(cand) >= urllen)
			continue;   /* see the header: only the Spotify CDN is in scope */
		snprintf(url, urllen, "%s", cand);
		break;
	}
	http_reply_free(&r);
	if (!url[0]) {
		snprintf(err, errlen, "the account has no usable picture");
		return -1;
	}
	return 0;
}

int sp_library_modify(const char *token, const char *track_id, bool save,
                      char *err, size_t errlen, int *retry_after)
{
	if (retry_after)
		*retry_after = 0;
	char path[96];
	snprintf(path, sizeof(path), "/v1/me/tracks?ids=%s", track_id);
	http_reply r;
	memset(&r, 0, sizeof(r));
	/* The ids query form takes an empty body; the JSON body form would need
	 * a Content-Type the tiny client does not bother with elsewhere. */
	if (http_request("api.spotify.com", path, save ? "PUT" : "DELETE",
	                 token, NULL, NULL, NULL, 0, 0, &r) != 0) {
		snprintf(err, errlen, "like: %s", r.err);
		http_reply_free(&r);
		return -1;
	}
	if (r.status == 429 && retry_after)
		*retry_after = r.retry_after;
	if (r.status != 200) {
		snprintf(err, errlen, "like: HTTP %d", r.status);
		http_reply_free(&r);
		return r.status == 403 ? 1 : -1;   /* 1: missing scope, re-pair */
	}
	http_reply_free(&r);
	return 0;
}
