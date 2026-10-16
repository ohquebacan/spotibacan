/* The native Spotify source for the existing client.
 *
 * audio3ds was built around the bridge: it asks a server on the LAN for a list
 * and gets JSON back. This offers the same shape straight from Spotify, so the
 * UI -- skins, covers, the system keyboard, the touch list -- does not change.
 *
 * Tracks stream: the whole file is reserved up front, a download thread fills
 * it in the background, and the decoder walks behind the download watermark
 * without ever overtaking it. */
#ifndef NATIVE_H
#define NATIVE_H

#include <stdbool.h>
#include <stddef.h>

#define NATIVE_ROWS 512         /* a whole list: liked songs easily pass 300 */

typedef enum {
	NATIVE_PLAYLISTS,           /* the account's playlist folder */
	NATIVE_LIKED,
	NATIVE_SEARCH,              /* uses the query */
	NATIVE_RADIO,               /* seeded from the selected track */
	NATIVE_TRACKS,              /* inside one playlist */
	NATIVE_ALBUM,               /* the playing track's album */
	NATIVE_ARTIST,              /* the playing track's artist */
	NATIVE_DOWNLOADS,           /* local files; the one view that works offline */
} native_view;

typedef struct {
	char label[64];             /* what the row shows */
	char sub[40];               /* artist, or the track count */
	char uri[192];              /* spotify:track:... or file:<name>.ogg */
	char art[200];               /* 64px cover URL, empty when there is none */
	char artist_uri[40];        /* spotify:artist:... for the context menu */
	char album_uri[40];         /* spotify:album:... for the context menu */
	bool is_playlist;
} native_row;

/* Where decoded audio goes. The client already has a ring that NDSP drains,
 * so the native player is just another producer for it and the whole audio
 * path downstream stays exactly as it was. */
typedef struct {
	void (*write)(const unsigned char *pcm, unsigned len);
	unsigned (*free_space)(void);
} native_sink;

/* Pairs the console with an account when there is no stored token.
 *
 * The code has to stay on screen while the user walks to another device and
 * types it in, so this cannot be a step inside native_init: the caller draws
 * it, and this reports progress through the hook. `show_code` is called once
 * with the code and the url; `poll` is called between attempts and returns
 * false to give up. Returns 0 once a token is stored, or -1. */
int  native_pair(void (*show_code)(const char *code, const char *url),
                 bool (*poll)(int attempt), char *err, size_t errlen);

/* Whether a stored token exists, i.e. whether pairing can be skipped. */
bool native_has_token(void);

/* Signs in: stored refresh token -> OAuth -> access point -> login5 ->
 * spclient. Slow enough to need a screen of its own (a second or two), so the
 * caller passes a hook that reports each step. Returns 0, or -1 with err set.
 *
 * The access point session is kept open afterwards: audio keys come from
 * there and nowhere else, and reopening it per track would cost a second
 * each time. The player thread answers its pings while idle. */
int  native_init(const native_sink *sink, void (*progress)(const char *step),
                 char *err, size_t errlen);

/* Brings up just enough to play the files under music/: no sign-in, no
 * network, but the same player thread. Used when init fails for want of
 * internet, so the downloads stay reachable on the road. */
int  native_init_offline(const native_sink *sink);
bool native_offline(void);
void native_exit(void);
bool native_ready(void);

/* Fills up to `max` rows for a view. `arg` is the search query for
 * NATIVE_SEARCH and the playlist or seed URI for NATIVE_TRACKS / NATIVE_RADIO,
 * and is ignored otherwise. Returns the row count, or -1 with err set. */
int  native_fetch(native_view view, const char *arg, native_row *rows, int max,
                  char *err, size_t errlen);

/* Asks the in-flight fetch to stop at the next chunk boundary (about a
 * second): for navigation away from a view whose rows are still arriving.
 * The next fetch clears the flag on entry. */
void native_fetch_cancel(void);

/* Progress hook for long list fetches: fires after each named chunk with the
 * staging rows filled so far. The UI commits them progressively, so a 500-
 * song list shows its first 50 after one round trip instead of sitting on a
 * spinner until the last chunk lands. */
void native_set_progress_cb(void (*cb)(native_row *rows, int done, int total));

/* ------------------------------------------------------------- playback */

/* Asks for a track. Returns immediately: resolving, fetching the key and
 * downloading take seconds, and none of that belongs on the render loop. */
void native_play(const char *track_uri);
void native_stop(void);

typedef enum {
	NATIVE_IDLE,
	NATIVE_LOADING,
	NATIVE_PLAYING,
	NATIVE_FAILED,
} native_state;

native_state native_playback_state(void);
const char  *native_now_title(void);
const char  *native_now_artist(void);
const char  *native_now_album(void);
/* 300px cover of the playing track, or "" -- the UI keeps its placeholder
 * until this has something to say. */
const char  *native_now_art(void);
/* The user's own avatar URL, for the idle hero art. Empty when the account
 * has no usable image. */
const char  *native_profile_art(void);
/* Fetches the avatar URL once; the endpoint is rate-limited often enough
 * (HTTP 429) that the art worker makes a single attempt and then stops. */
int          native_profile_art_refresh(void);
/* The URI last asked for; empty before the first play. */
const char  *native_now_uri(void);
/* Where "go to album / go to artist" navigates for the playing track;
 * empty for local files or when the metadata carried no gid. */
const char  *native_now_album_uri(void);
const char  *native_now_artist_uri(void);
const char  *native_last_error(void);
/* Likes/unlikes a track URI ("Me gusta"). 0 done, 1 missing scope
 * (re-pair needed), -1 other failure; on 429 *retry_after gets the server's
 * seconds. Runs from the art worker. */
int          native_like(const char *uri, bool like, char *err,
                         size_t errlen, int *retry_after);
/* When on, fully downloaded tracks are also written to the SD as .ogg. */
void         native_set_downloads(bool on);
/* The player's download button: arms/disarms saving the track now playing
 * once its background download completes; armed state for the icon. */
void         native_save_current(void);
bool         native_save_armed(void);
/* True once the playing track's .ogg exists on the card (cached per URI). */
bool         native_current_saved(void);

/* ------------------------------------------------- download queue */
/* An armed download that gets cut short (its background slot is taken by a
 * newer one) is queued here instead of dying: the player drains the queue
 * one track at a time whenever no other download is using the link. */
#define NATIVE_DLQ_MAX 12

/* Queue length for the badge, and entry i's title for the popup
 * (0 = the oldest). Returns false when i is out of range. */
int  native_dl_queue_count(void);
bool native_dl_queue_entry(int i, char *title, size_t titlelen);
/* The download the queue is running right now: 0-100, or -1 when idle.
 * title_out gets its name when active. */
int  native_dl_active(char *title_out, size_t titlelen);
/* Appends a line to the same native.log the backend writes; for UI-side
 * failures that would otherwise leave no trace (a stuck view, a dead tap). */
void         native_log(const char *fmt, ...);
/* Seconds of audio handed to the ring, and the track's length. */
float        native_position(void);
float        native_duration(void);
/* Ask the decoder to jump to `seconds`. Honored only inside the downloaded
 * span; a jump past the watermark is ignored. */
void         native_seek(float seconds);

/* A playlist's title. Fetched when it is opened, not for every row of the
 * rootlist: that would be one request per row for names most of which the
 * user never looks at. */
int  native_playlist_name(const char *uri, char *name, size_t namelen);

/* The account name the access point reported, for the header. */
const char *native_account(void);

#endif
