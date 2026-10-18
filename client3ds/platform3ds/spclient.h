/* spclient: Spotify's internal API, the one that answers for this client.
 *
 * Needs a login5 Bearer token -- the OAuth token is refused here, and
 * api.spotify.com answers 429 for the desktop client id, so this is the only
 * way in. Everything speaks protobuf except a few endpoints that answer JSON.
 */
#ifndef SPCLIENT_H
#define SPCLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct http_session http_session;

#define SP_MAX_FILES 8

/* Audio formats, from metadata.proto's AudioFile.Format. Only the Vorbis ones
 * matter here: the 3DS decodes Vorbis in software and has no AAC decoder worth
 * using. 160 kbps is the one to prefer -- 320 triples the bandwidth over
 * 802.11g for a difference the console's speakers cannot show. */
#define SP_FMT_OGG_96   0
#define SP_FMT_OGG_160  1
#define SP_FMT_OGG_320  2

typedef struct {
	uint8_t id[20];
	int     format;
} sp_file;

typedef struct {
	char    uri[64];
	/* The track's raw 16-byte id. The audio key request wants this, not the
	 * URI -- it is the same value the base62 in the URI decodes to. */
	uint8_t gid[16];
	bool    has_gid;
	char    name[128];
	char    artist[128];
	char    album[128];
	/* Cover art as full i.scdn.co URLs: small (64px) for list rows, large
	 * (300px) for the now-playing screen. Empty when the album carries no
	 * images. */
	char    art_small[80];
	char    art_large[80];
	/* spotify:album:... and spotify:artist:... (first artist), built from
	 * the gids, for "go to album / artist". Empty when absent. */
	char    album_uri[40];
	char    artist_uri[40];
	int     duration_ms;
	sp_file files[SP_MAX_FILES];
	int     file_count;
} sp_track;

/* Which spclient host to use. Falls back to the well-known name when apresolve
 * has nothing to say, which is what librespot does too. */
int  sp_resolve(char *host, size_t hostlen, char *err, size_t errlen);

/* One track's metadata, via the extended-metadata batch service. */
int  sp_track_metadata(const char *host, const char *token, const char *track_uri,
                       sp_track *out, char *err, size_t errlen);
/* The same over a caller-owned keep-alive session (NULL = fresh connection). */
int  sp_track_metadata_s(http_session *sess, const char *host, const char *token,
                         const char *track_uri, sp_track *out, char *err,
                         size_t errlen);

/* Several at once. The service takes a repeated entity_request, so a screenful
 * of tracks costs one round trip instead of one each -- which on this console
 * is the difference between a list that fills instantly and one that crawls.
 * Returns how many were filled in, or -1. */
int  sp_track_metadata_batch(const char *host, const char *token,
                             const char *const *uris, int n,
                             sp_track *out, char *err, size_t errlen);

/* Same batch over a keep-alive session: chunk after chunk of a long list
 * pays one handshake total instead of one each. */
int  sp_track_metadata_batch_s(http_session *sess, const char *token,
                               const char *const *uris, int n,
                               sp_track *out, char *err, size_t errlen);

/* Searching the catalogue. Needs no library, which is what makes a fresh
 * account usable: the answer is a list of track URIs, which
 * sp_track_metadata_batch then turns into names.
 * `query` is plain text; spaces become '+'. */
int  sp_search_tracks(const char *host, const char *token, const char *query,
                      char uris[][64], int max, char *err, size_t errlen);

/* Recommendations seeded from one track. Unlike search, this answers with the
 * titles and artists inline, so no metadata round trip is needed at all. */
int  sp_radio(const char *host, const char *token, const char *seed_uri,
              int count, sp_track *out, int max, char *err, size_t errlen);

/* The user's playlist folder. `count` is how many entries the rootlist holds;
 * up to `max` of their URIs are copied into `uris`. An empty rootlist is a
 * valid answer, not an error. */
/* The playlist folder, with each list's name.
 *
 * Asking for the decorated rootlist costs the same one request and brings the
 * names along in meta_items; without it the rows have nothing to show but an
 * index, and resolving each playlist for its title would be one request per
 * row. `names` may be NULL. `arts`, when given, gets each row's cover URL
 * (64px-thumb shape) or an empty string; pass NULL to skip. */
int  sp_rootlist(const char *host, const char *token, const char *username,
                 char uris[][64], char names[][64], char arts[][80], int max,
                 int *count, char *err, size_t errlen);

/* The tracks in one playlist. `name` gets the playlist's title, `uris` up to
 * `max` track URIs, and `count` the length the server reports, which can
 * exceed what fits. Returns how many URIs were read, or -1.
 *
 * Same SelectedListContent as the rootlist: a rootlist holds playlist URIs and
 * a playlist holds track URIs, so one parser serves both. */
int  sp_playlist(const char *host, const char *token, const char *playlist_id,
                 char *name, size_t namelen, char uris[][64], int max,
                 int *count, char *err, size_t errlen);

/* Liked Songs. This one answers JSON, not protobuf, and carries the whole
 * first page inline. An empty page means an empty library, not a failure --
 * which is worth knowing, because that is what it looked like before this
 * account had anything saved. */
int  sp_liked(const char *host, const char *token, const char *username,
              char uris[][64], int max, char *err, size_t errlen);

/* Where the CDN is serving one audio file from. Up to three signed URLs, each
 * with an expiry; any of them serves the same bytes. */
#define SP_MAX_CDN 3

typedef struct {
	char urls[SP_MAX_CDN][512];
	int  count;
	int  result;                 /* 0 = CDN, 1 = storage, 3 = restricted */
} sp_storage;

/* `file_id` is the 20 raw bytes from sp_file. */
int  sp_storage_resolve(const char *host, const char *token,
                        const uint8_t file_id[20], sp_storage *out,
                        char *err, size_t errlen);
/* An artist's albums and singles, newest Spotify order, for the artist
 * view. type is 1 for an album, 2 for a single/EP. */
typedef struct {
	char uri[40];
	char name[64];
	char art[80];       /* 64px cover, "" when none */
	int  year;          /* 0 when unknown */
	int  type;
} sp_release;

int  sp_artist_releases(http_session *sess, const char *host, const char *token,
                        const char *artist_uri, sp_release *out, int max,
                        char *err, size_t errlen);

/* The tracks of any context the context service knows: an album, or an
 * artist (its top tracks). Returns the count, or -1 with err set. */
/* Appends a track to a playlist the account may edit, through the same
 * playlist service the official clients use (ListChanges on the current
 * revision). 0 on success; err says why not (e.g. "not your playlist"). */
int  sp_playlist_add(const char *host, const char *token,
                     const char *playlist_uri, const char *track_uri,
                     char *err, size_t errlen);

int  sp_context_tracks(const char *host, const char *token, const char *uri,
                       char uris[][64], int max, char *err, size_t errlen);

int  sp_storage_resolve_s(http_session *sess, const char *host,
                          const char *token, const uint8_t file_id[20],
                          sp_storage *out, char *err, size_t errlen);

/* Picks the best file this client can actually play, or NULL. */
const sp_file *sp_pick_file(const sp_track *t);

/* Opt-in 320 kbps (Settings). Only taken when the whole file fits the
 * stream buffer (SP_HQ_MAX_BYTES, kept equal to TRACK_STREAM_MAX): a long
 * track quietly falls back to 160 rather than failing. */
#define SP_HQ_MAX_BYTES (12u * 1024u * 1024u)
void sp_set_high_quality(bool on);
/* sp_pick_file with an explicit choice; sp_pick_file follows the setting. */
const sp_file *sp_pick_file_q(const sp_track *t, bool hq);
bool sp_high_quality(void);
const char *sp_format_name(int format);

/* The user's profile image URL, from the Web API's /v1/me (the one service
 * that lives on api.spotify.com rather than spclient). Only i.scdn.co URLs
 * are accepted: the hero art buffer is 80 chars and the CA bundle only
 * covers Spotify's own CDN, so a lookaside avatar is reported as "none"
 * rather than half-supported. Returns 0, or -1 when the account has no
 * usable image. On HTTP 429, `retry_after` (when given) receives the
 * server's Retry-After seconds so the caller waits on its clock. */
int  sp_user_image(const char *token, char *url, size_t urllen,
                   char *err, size_t errlen, int *retry_after);

/* Adds or removes a track in the user's library ("Me gusta"): PUT/DELETE
 * /v1/me/tracks?ids=<id>. Needs the user-library-modify scope, which tokens
 * paired before it was added do not carry -- a 403 means re-pair. On 429
 * `retry_after` gets the server's seconds, like sp_user_image. */
int  sp_library_modify(const char *token, const char *track_id, bool save,
                       char *err, size_t errlen, int *retry_after);

#endif
