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

/* The base62 a 16-byte gid maps to, the same alphabet librespot writes:
 * big-endian 128-bit, divided down. The metadata answers carry gids, the
 * rest of the client speaks URIs, and this is the only bridge. */
static void base62_gid(const uint8_t gid[16], char out[23])
{
	static const char ABC[] =
	    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	uint8_t n[16];
	memcpy(n, gid, 16);
	char tmp[23];
	int ntmp = 0;
	for (int i = 0; i < 22; i++) {
		unsigned rem = 0;
		for (int j = 0; j < 16; j++) {
			unsigned cur = (unsigned)n[j] + (rem << 8);
			n[j] = (uint8_t)(cur / 62);
			rem = cur % 62;
		}
		tmp[ntmp++] = ABC[rem];
	}
	tmp[ntmp] = '\0';
	for (int i = 0; i < ntmp; i++)
		out[i] = tmp[ntmp - 1 - i];
	out[ntmp] = '\0';
}

static void uri_from_gid(char *out, size_t cap, const char *kind,
                         const uint8_t gid[16])
{
	char b62[23];
	base62_gid(gid, b62);
	snprintf(out, cap, "spotify:%s:%s", kind, b62);
}

/* The inverse of base62_gid: the base62 piece of a URI back into the raw
 * 16-byte gid. metadata/4 paths take the gid as lowercase hex, never the
 * base62 the URI shows -- asking with the base62 is what got the 404s.
 * Multiplication carries run toward the most significant byte, so the array
 * walks from index 15 up. */
static bool base62_to_gid(const char *b62, uint8_t gid[16])
{
	static const char ABC[] =
	    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	uint8_t n[16] = {0};
	for (const char *p = b62; *p; p++) {
		const char *q = strchr(ABC, *p);
		if (!q)
			return false;
		unsigned carry = (unsigned)(q - ABC);
		for (int j = 15; j >= 0; j--) {
			unsigned cur = (unsigned)n[j] * 62 + carry;
			n[j] = (uint8_t)(cur & 0xFF);
			carry = cur >> 8;
		}
		if (carry)
			return false; /* more than 128 bits: not a track gid */
	}
	memcpy(gid, n, 16);
	return true;
}

/* lowercase hex of a gid, the shape metadata/4 and the CDN both want */
static void gid_hex(const uint8_t gid[16], char out[33])
{
	for (int i = 0; i < 16; i++)
		snprintf(out + i * 2, 3, "%02x", gid[i]);
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
	const char *large = ids[2][0] ? ids[2] : ids[0][0] ? ids[0] : ids[1];
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

const sp_file *sp_pick_file(const sp_track *t)
{
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

int sp_track_metadata(const char *host, const char *token, const char *track_uri,
                      sp_track *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));

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
	if (http_request(host, "/extended-metadata/v0/extended-metadata", "POST",
	                 token, "application/x-protobuf", "application/x-protobuf",
	                 req.p, req.len, 0, &r) != 0) {
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
		if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
			uri_from_gid(out->album_uri, sizeof(out->album_uri), "album",
			             an.p);
		if (pb_get_bytes(v.p, v.len, 2, &an))         /* Album.name */
			copy_str(out->album, sizeof(out->album), &an);
	}
	read_cover_urls(&track, out);
	if (pb_get_bytes(track.p, track.len, 4, &v)) {    /* first Artist */
		pbv an;
		/* Artist.gid is field 1 and Artist.name is field 2; reading the first
		 * field here would hand back raw bytes that are not text. */
		if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
			uri_from_gid(out->artist_uri, sizeof(out->artist_uri), "artist",
			             an.p);
		if (pb_get_bytes(v.p, v.len, 2, &an))
			copy_str(out->artist, sizeof(out->artist), &an);
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

	/* {"pages":[{"tracks":[{"uri":"spotify:track:...", "metadata":{...}}, ...]}]}
	 * The array iterator finds "tracks" wherever it sits, which saves walking
	 * into "pages" first. */
	int n = 0;
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (n < max && json_array_next(r.body, "tracks", &cursor, &obj, &objlen)) {
		char buf[512];
		if (objlen >= sizeof(buf))
			continue;
		memcpy(buf, obj, objlen);
		buf[objlen] = '\0';
		char uri[64];
		if (json_str(buf, "uri", uri, sizeof(uri)))
			snprintf(uris[n++], 64, "%s", uri);
	}
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
					if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
						uri_from_gid(out[filled].album_uri,
						             sizeof(out[filled].album_uri),
						             "album", an.p);
					if (pb_get_bytes(v.p, v.len, 2, &an))
						copy_str(out[filled].album, sizeof(out[filled].album), &an);
				}
				read_cover_urls(&track, &out[filled]);
				if (pb_get_bytes(track.p, track.len, 4, &v)) {
					pbv an;
					if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
						uri_from_gid(out[filled].artist_uri,
						             sizeof(out[filled].artist_uri),
						             "artist", an.p);
					if (pb_get_bytes(v.p, v.len, 2, &an))
						copy_str(out[filled].artist, sizeof(out[filled].artist), &an);
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
	 * as '+'. Everything else still has to be percent-encoded, since this all
	 * travels in a request line. */
	char plus[192];
	size_t n = 0;
	for (const char *p = query; *p && n < sizeof(plus) - 1; p++)
		plus[n++] = (*p == ' ') ? '+' : *p;
	plus[n] = '\0';

	/* No percent-encoding here: '+' and ':' are structural in this uri, and a
	 * query with anything stranger than letters, digits and spaces is not
	 * something this console's keyboard is going to produce. */
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

/* ----------------------------------------------------- album / artist views */

/* Fills one brief from a metadata Track entry: gid -> URI, name, first
 * artist, small cover. The same shapes the batch parser reads. */
static void track_brief(const pbv *track, sp_brief *b)
{
	pbv v;
	memset(b, 0, sizeof(*b));
	if (pb_get_bytes(track->p, track->len, 1, &v) && v.len == 16)
		uri_from_gid(b->uri, sizeof(b->uri), "track", v.p);
	if (pb_get_bytes(track->p, track->len, 2, &v))
		copy_str(b->name, sizeof(b->name), &v);
	if (pb_get_bytes(track->p, track->len, 3, &v)) {
		pbv an;
		if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
			uri_from_gid(b->album_uri, sizeof(b->album_uri), "album",
			             an.p);
	}
	if (pb_get_bytes(track->p, track->len, 4, &v)) {
		pbv an;
		if (pb_get_bytes(v.p, v.len, 1, &an) && an.len == 16)
			uri_from_gid(b->artist_uri, sizeof(b->artist_uri), "artist",
			             an.p);
		if (pb_get_bytes(v.p, v.len, 2, &an))
			copy_str(b->artist, sizeof(b->artist), &an);
	}
	sp_track tmp;
	memset(&tmp, 0, sizeof(tmp));
	read_cover_urls(track, &tmp);
	snprintf(b->art_small, sizeof(b->art_small), "%s", tmp.art_small);
}

static int brief_get(const char *host, const char *path, const char *token,
                     int disc_field, int top_field, sp_brief *out, int max,
                     char *name, size_t namelen, int *count,
                     char *err, size_t errlen)
{
	http_reply r;
	if (http_get_proto(host, path, token, &r) != 0) {
		snprintf(err, errlen, "metadata: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "metadata returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	int filled = 0, seen = 0;
	pbv v;
	if (name && pb_get_bytes(body, r.len, 2, &v))
		copy_str(name, namelen, &v);

	/* Album.disc (8) -> Disc.track (3); Artist.top_track (11) -> the
	 * group's track (2). Both repeated, so walked by hand: pb_get_bytes
	 * yields the first occurrence from the cursor, and the cursor advances
	 * past whatever it returned. */
	size_t at = 0;
	while (at < r.len) {
		pbv rest = { body + at, r.len - at };
		pbv group;
		if (!pb_get_bytes(rest.p, rest.len, disc_field, &group))
			break;
		size_t gat = 0;
		while (gat < group.len) {
			pbv grest = { group.p + gat, group.len - gat };
			pbv track;
			if (!pb_get_bytes(grest.p, grest.len, top_field, &track))
				break;
			if (track.len) {
				seen++;
				if (filled < max) {
					track_brief(&track, &out[filled]);
					if (out[filled].uri[0])
						filled++;
				}
			}
			gat = (size_t)(track.p + track.len - group.p);
		}
		at = (size_t)(group.p + group.len - body);
	}

	http_reply_free(&r);
	if (count)
		*count = seen;
	if (filled == 0 && seen == 0) {
		snprintf(err, errlen, "no tracks in a %u byte response",
		         (unsigned)r.len);
		return -1;
	}
	return filled;
}

/* The metadata/4 paths want the entity's gid as lowercase hex, not the
 * base62 the URI carries; a base62 in this path is what answered 404. Shared
 * shape for the album discs and the artist top-tracks calls. */
static int entity_brief_get(const char *host, const char *token, const char *kind,
                            const char *id, int disc_field, int top_field,
                            sp_brief *out, int max, char *name, size_t namelen,
                            int *count, char *err, size_t errlen)
{
	uint8_t gid[16];
	char hex[33];
	if (!base62_to_gid(id, gid)) {
		snprintf(err, errlen, "%s id '%s' is not base62", kind, id);
		return -1;
	}
	gid_hex(gid, hex);

	char path[160];
	snprintf(path, sizeof(path), "/metadata/4/%s/%s", kind, hex);
	return brief_get(host, path, token, disc_field, top_field, out, max,
	                 name, namelen, count, err, errlen);
}

int sp_album_tracks(const char *host, const char *token, const char *album_id,
                    sp_brief *out, int max, char *name, size_t namelen,
                    int *count, char *err, size_t errlen)
{
	return entity_brief_get(host, token, "album", album_id, 8, 3, out, max,
	                        name, namelen, count, err, errlen);
}

int sp_artist_top(const char *host, const char *token, const char *artist_id,
                  sp_brief *out, int max, char *name, size_t namelen,
                  int *count, char *err, size_t errlen)
{
	return entity_brief_get(host, token, "artist", artist_id, 11, 2, out, max,
	                        name, namelen, count, err, errlen);
}

/* --------------------------------------------------------- storage resolve */

int sp_storage_resolve(const char *host, const char *token,
                       const uint8_t file_id[20], sp_storage *out,
                       char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));

	char hex[41];
	for (int i = 0; i < 20; i++)
		snprintf(hex + i * 2, 3, "%02x", file_id[i]);

	char path[128];
	snprintf(path, sizeof(path),
	         "/storage-resolve/files/audio/interactive/%s", hex);

	http_reply r;
	if (http_get_proto(host, path, token, &r) != 0) {
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
		snprintf(err, errlen, "perfil: %s", r.err);
		http_reply_free(&r);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "perfil: HTTP %d", r.status);
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
		snprintf(err, errlen, "la cuenta no tiene foto usable");
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
		snprintf(err, errlen, "me gusta: %s", r.err);
		http_reply_free(&r);
		return -1;
	}
	if (r.status == 429 && retry_after)
		*retry_after = r.retry_after;
	if (r.status != 200) {
		snprintf(err, errlen, "me gusta: HTTP %d", r.status);
		http_reply_free(&r);
		return r.status == 403 ? 1 : -1;   /* 1: missing scope, re-pair */
	}
	http_reply_free(&r);
	return 0;
}
