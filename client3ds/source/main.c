// Native Spotify client for the 3DS.
//
// This grew out of a player that streamed raw PCM from a bridge on the LAN.
// The bridge is gone: the console now signs in to Spotify itself, browses the
// account and decodes Ogg Vorbis on the ARM11. What is left of that ancestry,
// and worth keeping, is the shape -- a ring buffer that NDSP drains, filled by
// whoever is producing audio, so the player and the audio path never have to
// know about each other.

#include <3ds.h>
#include <3ds/services/mcuhwc.h>
#include <3ds/services/ndm.h>
#include <citro2d.h>

#include "cfg3ds.h"
#include "lyrics3ds.h"
#include "native.h"
#include "spclient.h"
#include "eq.h"
#include "tls3ds.h"
#include "ui/artwork.h"
#include "ui/lang.h"
#include "ui/screen_list.h"
#include "ui/screen_lyrics.h"
#include "ui/screen_player.h"
#include "ui/thumbs.h"
#include "ui/visualizer.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "ui/volume_overlay.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SOC_BUFFER      0x100000
/* ~8.9 s at 44.1 kHz stereo. The cushion lets the download queue spend a
 * few blocking seconds starting its next file without an audible gap. */
#define RING_BYTES      (1536 * 1024)
#define WAVE_BUFFERS    4
#define FRAMES_PER_BUF  2048

typedef struct {
	u8 *data;
	volatile u32 head, tail;      // byte offsets; head == tail means empty
	LightLock lock;
} Ring;

static Ring ring;
static volatile bool running = true;
static volatile u32 underruns = 0;
// Spotify serves 44.1 kHz Vorbis and nothing else reaches the ring.
#define SAMPLE_RATE 44100
static char now_title[128] = "", now_artist[128] = "", now_album[128] = "";
static volatile bool art_pending = false, art_present = false;
static volatile bool server_playing = true;
static volatile float now_pos = 0.0f, now_dur = 0.0f;

// The list scrolls by pixels over the cached rows: sel is the absolute d-pad
// selection, scroll/velocity the touch physics.
typedef struct {
	bool open, loading;
	int total, sel;
	float scroll, velocity;
	char query[64];
} Library;
static Library lib;
static volatile bool lib_reload = false;

static native_view nat_view = NATIVE_PLAYLISTS;
/* A row uri: "dlist:<playlist title>.txt" outgrows 64 bytes. */
#define NAT_ARG_LEN 192
static char nat_arg[NAT_ARG_LEN] = "";
static char nat_title[48];
static native_row nat_rows[NATIVE_ROWS];
static int  nat_count = 0;
static char nat_status[64] = "";
// What the cached rows correspond to, so turning a page does not refetch.
static native_view nat_loaded_view = (native_view)-1;
static char nat_loaded_arg[NAT_ARG_LEN] = "\x01";

/* Fixed views get their title from the string table, so a language change
 * renames them; TRACKS keeps the playlist name it drilled into. */
static void refresh_nat_title(void)
{
	switch (nat_view) {
	case NATIVE_PLAYLISTS: snprintf(nat_title, sizeof(nat_title), "%s", tr(S_LISTS)); break;
	case NATIVE_LIKED:     snprintf(nat_title, sizeof(nat_title), "%s", tr(S_LIKED)); break;
	case NATIVE_RADIO:     snprintf(nat_title, sizeof(nat_title), "%s", tr(S_RADIO)); break;
	case NATIVE_SEARCH:    snprintf(nat_title, sizeof(nat_title), "%s", tr(S_SEARCH_VIEW)); break;
	case NATIVE_DOWNLOADS: snprintf(nat_title, sizeof(nat_title), "%s", tr(S_DL_VIEW)); break;
	default: break;
	}
}

// Touch is rebuilt every frame: each screen registers its hit rects as it
// draws, and the state machine consumes the previous frame's set.
static touch_state g_touch;
static touch_builder g_tb;

// START opens the settings menu instead of killing the music outright: an
// accidental press (a palm, a pocket) used to close the app mid-track.
static bool settings_open = false;
/* Battery for the settings header: percent from the MCU, refreshed every
 * few seconds; -1 until the first read succeeds. */
static int  batt_pct = -1;
static bool batt_charging = false;
/* Minutes left, estimated from how fast the percent has fallen since the
 * charger came off (or the app started); -1 while there is too little to
 * go on. The MCU gives no current draw, only the percent. */
static int  batt_left_min = -1;
/* The spectrum visualizer replaces the now-playing top screen while on. */
static bool vis_open = false;
static vis_style vis_mode = VIS_CLASSIC;
/* "Add to playlist": a picker over the cached playlist list, the add itself
 * done by the art worker (it is network), the outcome shown as a toast. */
static bool pick_open = false;
static native_row pick_rows[NATIVE_ROWS];
static int  pick_n = 0, pick_sel = 0;
static char pick_track[192];
static volatile bool add_req = false;
static char add_req_playlist[192], add_req_track[192], add_req_name[64];
static char toast[96];
static u64  toast_until = 0;

static void toast_show(const char *msg)
{
	snprintf(toast, sizeof(toast), "%s", msg);
	toast_until = osGetTime() + 3500;
}
/* What the playing list was called, for the status line on top. */
static char play_ctx_title[64] = "";
static int  settings_sel = 0;
/* Applying a preference writes the SD card, which stalls the frame long
 * enough to read as a freeze. So a tap only arms the change; the menu draws
 * with a busy hint, the write happens on the next frame, and the hint
 * lingers a moment so it reads as feedback rather than a blink. */
static int  settings_pending = -1;
static u64  settings_busy_until = 0;

// File scope because both the input loop and the drawers consult them.
static bool muted = false;
static bool paused = false;
/* While casting the console stays paused; the speaker's own state is
 * cast_paused, which is what the play button and status show. */
static bool casting(void);
static bool cast_paused;
/* The START cast row's last stop: "Find speakers", past the speakers. */
static bool cast_find_row = false;

// Playback modes and the downloads preference; the settings drawer and the
// transport both read them. The like queue lives with the files below.
static bool shuffle_on = false;
static int  repeat_mode = 0;   // 0 off, 1 all, 2 one
static bool downloads_on = false;

// Lid-closed playback. Closing the lid normally sleeps the console, and even
// awake it flips the wifi from infrastructure to StreetPass; either one kills
// the stream. While a track is playing or paused, sleep is refused and the
// ndm:u daemon is locked into infrastructure mode (the same recipe
// Universal-Updater uses for its downloads). The download queue holds it
// too: outside it the wifi sits in power save, and a queued list crawled at
// ~26 KB/s against ~100 while a song played. Released only after a few
// seconds with nothing going on, so the IDLE blip at each track handover
// does not let a closed lid sleep between songs.
static bool lid_mode = false;
#define LID_RELEASE_MS 15000

static void lid_mode_set(bool on)
{
	if (on == lid_mode)
		return;
	if (on) {
		if (R_FAILED(ndmuInit()))
			return;
		Result res = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
		if (R_SUCCEEDED(res))
			res = NDMU_LockState();
		if (R_FAILED(res)) {
			ndmuExit();
			return;
		}
		aptSetSleepAllowed(false);
		lid_mode = true;
	} else {
		aptSetSleepAllowed(true);
		NDMU_LeaveExclusiveState();
		NDMU_LockState();
		ndmuExit();
		lid_mode = false;
	}
}

// The row the playing track came from, and the view that row belongs to.
// Prev/next are only meaningful while the list still shows that view.
static int now_idx = -1;
static native_view now_view = NATIVE_PLAYLISTS;
static char now_arg[NAT_ARG_LEN] = "";

/* The playback context: a snapshot of the list the playing track came from.
 * Transport (prev/next, L/R, auto-advance) walks this copy, so it keeps
 * working while the user browses other lists. */
static char play_uris[NATIVE_ROWS][192];
static char play_art[NATIVE_ROWS][200];
static char play_labels[NATIVE_ROWS][64];
static char play_subs[NATIVE_ROWS][40];
static int play_count = 0;
static int play_idx = -1;

// The hardware slider owns the volume; the overlay only follows it. Mute
// remains a local mix toggle, orthogonal to the slider.
static int volume = 100;        // display value, mirrored from the slider
static u8 vol_slider = 0xFF;    // last raw slider level seen (0..63)
static int vol_hold = 0;
static u8 vol_overlay = 0;

// Lyrics, filled by the art worker: the request carries what LRCLIB wants,
// the doc only becomes readable when lyr_state says so.
static bool lyr_open = false;
static lyrics_doc lyr_doc;
static volatile int lyr_state = LYR_ST_NONE;
static char lyr_error[160] = "";
static char lyr_track_uri[64] = "";
static volatile bool lyr_want = false;
static char lyr_w_track[128], lyr_w_artist[128], lyr_w_album[128];
static long lyr_w_dur = 0;
static float lyr_scroll = 0.0f;

// Audio runs on its own thread: the system keyboard suspends the main loop,
// and four wave buffers hold only a quarter second.
static volatile bool buffering = true;
static volatile u32 prebuffer_bytes = 0;
static ndspWaveBuf waves[WAVE_BUFFERS];
static u8 *art_staging = NULL;

#define ART_PIXELS 128
#define ART_BYTES  (ART_PIXELS * ART_PIXELS * 2)

// One dark palette, in code. The theme loader, the Rockbox backdrops and the
// .wps interpreter belonged to the local-music player this grew out of; a
// streaming client has no use for sixteen looks, and they cost 12 MB on the
// card to carry.
typedef struct {
	u32 top_bg, bottom_bg, panel, accent, on_accent, text, dim, meter, warm;
	bool dark;
} Skin;

#define RGB(r, g, b) C2D_Color32((r), (g), (b), 0xFF)

static Skin SKIN;

/* Accent choices for the settings menu. accent_keys doubles as the cfg
 * value; the display name is translated. RGBC is the constant-foldable
 * twin of RGB (C2D_Color32 is an inline function, illegal in a static
 * initialiser). */
#define RGBC(r, g, b) \
	((u32)0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))
#define ACCENT_COUNT 4
static const u32 ACCENTS[ACCENT_COUNT] = {
	RGBC(0x3D, 0xE8, 0x5B),   // green, the action bar's own
	RGBC(0xB8, 0x8A, 0xF5),   // purple
	RGBC(0x55, 0xC0, 0xF0),   // blue
	RGBC(0xF5, 0x96, 0x4A),   // orange
};
static const char *const ACCENT_KEYS[ACCENT_COUNT] =
	{"green", "purple", "blue", "orange"};
static const str_id ACCENT_NAMES[ACCENT_COUNT] =
	{S_ACCENT_GREEN, S_ACCENT_PURPLE, S_ACCENT_BLUE, S_ACCENT_ORANGE};
static int accent_idx = 0;

static void apply_accent(void)
{
	SKIN.accent = ACCENTS[accent_idx];
}

static void skin_init(void)
{
	SKIN.top_bg    = RGB(0x12, 0x14, 0x13);
	SKIN.bottom_bg = RGB(0x1B, 0x1E, 0x1C);
	SKIN.panel     = RGB(0x26, 0x2A, 0x27);
	SKIN.on_accent = RGB(0x0E, 0x14, 0x10);
	SKIN.text      = RGB(0xF2, 0xF4, 0xF2);
	SKIN.dim       = RGB(0x8E, 0x96, 0x90);
	SKIN.meter     = RGB(0x33, 0x39, 0x35);
	SKIN.warm      = RGB(0xFF, 0x8A, 0x70);   // headings, against all the green
	SKIN.dark      = true;
	apply_accent();
}

// UI preferences, kept beside the token so they survive reinstalls.
static void prefs_load(void)
{
	char v[16];
	if (cfg_get("lang", v, sizeof(v)))
		lang_set(strcmp(v, "en") == 0 ? LANG_EN : LANG_ES);
	if (cfg_get("accent", v, sizeof(v)))
		for (int i = 0; i < ACCENT_COUNT; i++)
			if (strcmp(v, ACCENT_KEYS[i]) == 0) {
				accent_idx = i;
				apply_accent();
				break;
			}
	if (cfg_get("downloads", v, sizeof(v)) && v[0] == '1')
		downloads_on = true;
	if (cfg_get("quality", v, sizeof(v)) && strcmp(v, "320") == 0)
		sp_set_high_quality(true);
	if (cfg_get("eq", v, sizeof(v)))
		eq_set(eq_from_key(v));
}

// ---------------------------------------------------------------- ring buffer

static u32 ring_used(void)
{
	u32 h = ring.head, t = ring.tail;
	return (h >= t) ? (h - t) : (RING_BYTES - t + h);
}

static void ring_write(const u8 *src, u32 len)
{
	// Copy in spans. Byte-at-a-time under the lock burned CPU the ARM11
	// cannot spare while the DSP and the socket both want servicing.
	LightLock_Lock(&ring.lock);
	u32 free_bytes = RING_BYTES - 1 - ring_used();
	if (len > free_bytes) len = free_bytes;   // full: drop the excess
	u32 first = RING_BYTES - ring.head;
	if (first > len) first = len;
	memcpy(ring.data + ring.head, src, first);
	if (len > first) memcpy(ring.data, src + first, len - first);
	ring.head = (ring.head + len) % RING_BYTES;
	LightLock_Unlock(&ring.lock);
}

static void ring_clear(void)
{
	LightLock_Lock(&ring.lock);
	ring.tail = ring.head;
	LightLock_Unlock(&ring.lock);
}

static u32 ring_read(u8 *dst, u32 len)
{
	LightLock_Lock(&ring.lock);
	u32 have = ring_used();
	if (len > have) len = have;
	u32 first = RING_BYTES - ring.tail;
	if (first > len) first = len;
	memcpy(dst, ring.data + ring.tail, first);
	if (len > first) memcpy(dst + first, ring.data, len - first);
	ring.tail = (ring.tail + len) % RING_BYTES;
	LightLock_Unlock(&ring.lock);
	return len;
}

// ---------------------------------------------------------------------- http

static void audio_thread(void *arg)
{
	(void)arg;
	const u32 buf_bytes = FRAMES_PER_BUF * 4;
	while (running) {
		if (buffering && ring_used() >= prebuffer_bytes)
			buffering = false;
		if (!buffering) for (int i = 0; i < WAVE_BUFFERS; i++) {
			if (waves[i].status != NDSP_WBUF_DONE && waves[i].status != NDSP_WBUF_FREE)
				continue;
			if (ring_used() < buf_bytes) {
				underruns++;
				buffering = true;
				break;
			}
			u8 *dst = (u8 *)waves[i].data_vaddr;
			u32 got = ring_read(dst, buf_bytes);
			if (got < buf_bytes) memset(dst + got, 0, buf_bytes - got);
			eq_process((int16_t *)dst, FRAMES_PER_BUF);
			vis_feed(i, (const int16_t *)dst, FRAMES_PER_BUF);
			DSP_FlushDataCache(dst, buf_bytes);
			ndspChnWaveBufAdd(0, &waves[i]);
		}
		svcSleepThread(5000000LL);   // 5 ms
	}
}

static u64 lib_retry_at = 0;   /* ms; when a failed view should refetch */

/* A small in-memory cache of whole views. Round trips are the expensive
 * part on this console -- a 500-song list is a dozen sequential TLS
 * handshakes -- so going back to a list you just opened should not pay
 * them again. Entries live ten minutes; the Downloads shelf is never
 * cached, it is local and must reflect the card. LRU by age. */
#define VIEW_CACHE_MAX 6
#define VIEW_CACHE_TTL_MS (20 * 60 * 1000)
static struct {
	native_view view;
	char arg[NAT_ARG_LEN];
	int count;
	unsigned long long at;   /* osGetTime() when stored, 0 = empty */
	native_row rows[NATIVE_ROWS];
} vcache[VIEW_CACHE_MAX];

/* Copies a fresh cached view into nat_rows and returns its row count,
 * or -1 on a miss. */
static int vcache_lookup(native_view view, const char *arg)
{
	if (view == NATIVE_DOWNLOADS)
		return -1;
	const unsigned long long now = osGetTime();
	for (int i = 0; i < VIEW_CACHE_MAX; i++) {
		if (!vcache[i].at || vcache[i].view != view ||
		    strcmp(vcache[i].arg, arg ? arg : "") != 0)
			continue;
		if (now - vcache[i].at > VIEW_CACHE_TTL_MS) {
			native_log("VCACHE: expiro vista=%d arg=%s\n", (int)view,
			           arg ? arg : "-");
			return -1;
		}
		nat_count = 0;   /* hidden while rewritten; the caller publishes */
		__sync_synchronize();
		memcpy(nat_rows, vcache[i].rows,
		       (size_t)vcache[i].count * sizeof(native_row));
		__sync_synchronize();
		lib.loading = false;   /* no spinner: the rows are already here */
		native_log("VCACHE: hit vista=%d arg=%s (%d filas)\n", (int)view,
		           arg ? arg : "-", vcache[i].count);
		return vcache[i].count;
	}
	native_log("VCACHE: miss vista=%d arg=%s\n", (int)view, arg ? arg : "-");
	return -1;
}

static void vcache_store(native_view view, const char *arg, int count)
{
	if (view == NATIVE_DOWNLOADS || count <= 0)
		return;
	int slot = 0;
	for (int i = 0; i < VIEW_CACHE_MAX; i++) {
		if (vcache[i].view == view &&
		    strcmp(vcache[i].arg, arg ? arg : "") == 0) {
			slot = i;
			break;
		}
		if (!vcache[i].at ||
		    (vcache[slot].at && vcache[i].at < vcache[slot].at))
			slot = i;
	}
	vcache[slot].view = view;
	snprintf(vcache[slot].arg, sizeof(vcache[slot].arg), "%s",
	         arg ? arg : "");
	vcache[slot].count = count;
	vcache[slot].at = osGetTime();
	memcpy(vcache[slot].rows, nat_rows,
	       (size_t)count * sizeof(native_row));
	native_log("VCACHE: guardada vista=%d arg=%s (%d filas, slot %d)\n",
	           (int)view, arg ? arg : "-", count, slot);
}

/* The in-flight fetch's identity, for the progressive commit below. */
static native_view fetch_view = (native_view)-1;
static char fetch_arg[NAT_ARG_LEN] = "";
static volatile int fetch_total = 0;

/* Progressive commit: each chunk that lands becomes visible at once, so a
 * long list starts answering after the first round trip. Dropped the moment
 * the user navigates away -- the cancel kills the fetch and the discard path
 * invalidates what was shown. */
static void list_progress(native_row *rows, int done, int total)
{
	const char *cur = nat_view == NATIVE_SEARCH ? lib.query : nat_arg;
	if (fetch_view != nat_view || strcmp(fetch_arg, cur ? cur : "") != 0)
		return;
	memcpy(nat_rows, rows, (size_t)done * sizeof(native_row));
	nat_count = done;
	nat_loaded_view = fetch_view;
	snprintf(nat_loaded_arg, sizeof(nat_loaded_arg), "%s", fetch_arg);
	fetch_total = total;
	lib.total = nat_count + (fetch_view == NATIVE_PLAYLISTS ? 2 : 0);
}

// Pulls one view from Spotify and caches the rows. The fetch is cached by
// view and argument: scrolling must not cost a round trip, because on this
// console round trips are the expensive part.
static void library_fetch(void)
{
	/* Snapshot the request: the fetch takes seconds and the user may
	 * navigate away in the middle, so what nat_view says at the end is not
	 * what it said at the start. */
	const native_view view = nat_view;
	char arg[NAT_ARG_LEN];
	snprintf(arg, sizeof(arg), "%s",
	         view == NATIVE_SEARCH ? lib.query : nat_arg);
	bool same = view == nat_loaded_view &&
	            strcmp(arg, nat_loaded_arg) == 0;

	if (!same) {
		int cached = vcache_lookup(view, arg);
		if (cached >= 0) {
			nat_count = cached;
			nat_status[0] = '\0';
			nat_loaded_view = view;
			snprintf(nat_loaded_arg, sizeof(nat_loaded_arg), "%s", arg);
			lib_retry_at = 0;
		} else {
			lib.loading = true;
			lib_retry_at = 0;
			fetch_total = 0;
			fetch_view = view;
			snprintf(fetch_arg, sizeof(fetch_arg), "%s", arg);
			char err[160] = "";
			/* Fetch into staging, never nat_rows: a fetch whose view is
			 * discarded (the user navigated away mid-flight) must leave
			 * the rows of whatever IS on screen untouched -- writing
			 * straight into nat_rows painted downloads over the playlist
			 * rows while nat_count still described the old view. */
			static native_row stage[NATIVE_ROWS];
			int n = native_fetch(view, arg, stage, NATIVE_ROWS,
			                     err, sizeof(err));
			/* The user moved on mid-fetch (a cancel also lands here):
			 * results for a view nobody is showing must never be
			 * committed, or the new view would draw these rows. The
			 * pending reload fetches what is actually on screen. */
			const char *cur =
			    nat_view == NATIVE_SEARCH ? lib.query : nat_arg;
			if (view != nat_view ||
			    strcmp(arg, cur ? cur : "") != 0) {
				native_log("LISTA: fetch de %d descartado (navegacion)\n",
				           (int)view);
				/* Partial rows may be on screen from the progressive
				 * commit: invalidate so coming back refetches the whole
				 * view rather than trusting the fragment. */
				if (nat_loaded_view == view)
					nat_loaded_view = (native_view)-1;
				return;
			}
			if (n < 0) {
				nat_count = 0;
				snprintf(nat_status, sizeof(nat_status), "%s", err);
				/* Mark the failed view as loaded anyway: leaving the
				 * mismatch against nat_loaded_view keeps the screen on
				 * "Cargando" forever and the error is never shown. The
				 * status line says what happened, and the retry gives a
				 * dropped session ten seconds to come back before trying
				 * again. */
				nat_loaded_view = view;
				snprintf(nat_loaded_arg, sizeof(nat_loaded_arg), "%s",
				         arg);
				lib_retry_at = osGetTime() + 10000;
				native_log("LISTA: fallo el fetch de la vista: %s\n", err);
			} else {
				/* The UI draws from another core now: hide the rows while
				 * they are rewritten, publish the count after. */
				nat_count = 0;
				__sync_synchronize();
				memcpy(nat_rows, stage,
				       (size_t)n * sizeof(native_row));
				__sync_synchronize();
				nat_count = n;
				nat_status[0] = '\0';
				nat_loaded_view = view;
				snprintf(nat_loaded_arg, sizeof(nat_loaded_arg), "%s",
				         arg);
				vcache_store(view, arg, n);
			}
		}
	}

	// The playlists view carries two synthetic rows on top of the rootlist:
	// Liked Songs first, then Downloads (local files, the offline path).
	lib.total = nat_count + (nat_view == NATIVE_PLAYLISTS ? 2 : 0);
	if (lib.sel >= lib.total) lib.sel = lib.total ? lib.total - 1 : 0;
	lib.loading = false;
}

// A library fetch takes about a second against Spotify, so it never runs on
// the render loop.
static void native_library_thread(void *arg)
{
	(void)arg;
	while (running) {
		if (lib_reload) {
			lib_reload = false;
			library_fetch();
		} else if (lib_retry_at && osGetTime() >= lib_retry_at) {
			lib_retry_at = 0;
			/* The failed view marked itself loaded so its error could show,
			 * so a plain reload would be a no-op; invalidate the cache tag
			 * to force the refetch. */
			nat_loaded_view = (native_view)-1;
			lib_reload = true;
		}
		svcSleepThread(30000000LL);   // 30 ms
	}
}

// The library heart. The like is optimistic: the heart fills at once, the
// PUT rides a queue the art worker drains when the rate-limited Web API has
// a window (same 429 quota as /v1/me).

#define LIKE_MAX 64
static char liked_ids[LIKE_MAX][40];
static int  liked_n = 0;
typedef struct { char uri[64]; bool like; } like_op;
static like_op like_pending[LIKE_MAX];
static int  like_pending_n = 0;
static bool like_scope_missing = false;
static int  like_wait_s = 0;

static void likes_file(char *out, size_t n, const char *name)
{
	char d[320];
	cfg_dir(d, sizeof(d));
	snprintf(out, n, "%s%s", d, name);
}

static void liked_load(void)
{
	char path[352];
	likes_file(path, sizeof(path), "liked.txt");
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[48];
	while (liked_n < LIKE_MAX && fgets(line, sizeof(line), f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = '\0';
		if (line[0])
			snprintf(liked_ids[liked_n++], 40, "%s", line);
	}
	fclose(f);
}

static void liked_save(void)
{
	char path[352], tmp[360];
	likes_file(path, sizeof(path), "liked.txt");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < liked_n; i++)
		fprintf(f, "%s\n", liked_ids[i]);
	fclose(f);
	rename(tmp, path);
}

static void like_pending_load(void)
{
	char path[352];
	likes_file(path, sizeof(path), "likes_pending.txt");
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[80];
	while (like_pending_n < LIKE_MAX && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = '\0';
		snprintf(like_pending[like_pending_n].uri, 64, "%s", line);
		like_pending[like_pending_n].like = tab[1] == '1';
		like_pending_n++;
	}
	fclose(f);
}

static void like_pending_save(void)
{
	char path[352], tmp[360];
	likes_file(path, sizeof(path), "likes_pending.txt");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < like_pending_n; i++)
		fprintf(f, "%s\t%c\n", like_pending[i].uri,
		        like_pending[i].like ? '1' : '0');
	fclose(f);
	rename(tmp, path);
}

static bool uri_is_liked(const char *uri)
{
	if (!uri || !*uri)
		return false;
	for (int i = 0; i < liked_n; i++)
		if (strcmp(liked_ids[i], uri) == 0)
			return true;
	return false;
}

static void liked_set(const char *uri, bool like)
{
	int at = -1;
	for (int i = 0; i < liked_n; i++)
		if (strcmp(liked_ids[i], uri) == 0) {
			at = i;
			break;
		}
	if (like && at < 0 && liked_n < LIKE_MAX)
		snprintf(liked_ids[liked_n++], 40, "%s", uri);
	else if (!like && at >= 0) {
		memmove(&liked_ids[at], &liked_ids[at + 1],
		        (size_t)(liked_n - at - 1) * sizeof(liked_ids[0]));
		liked_n--;
	}
	liked_save();
}

/* Queue the Web API call; the heart has already moved. A newer tap on the
 * same track replaces the queued one. */
static void like_toggle(const char *uri)
{
	if (!uri || !*uri)
		return;
	const bool like = !uri_is_liked(uri);
	liked_set(uri, like);
	/* The Liked list changed: its cache entry is a lie now. */
	for (int i = 0; i < VIEW_CACHE_MAX; i++)
		if (vcache[i].view == NATIVE_LIKED)
			vcache[i].at = 0;
	for (int i = 0; i < like_pending_n; i++)
		if (strcmp(like_pending[i].uri, uri) == 0) {
			memmove(&like_pending[i], &like_pending[i + 1],
			        (size_t)(like_pending_n - i - 1) * sizeof(like_pending[0]));
			like_pending_n--;
			break;
		}
	if (like_pending_n < LIKE_MAX) {
		snprintf(like_pending[like_pending_n].uri, 64, "%s", uri);
		like_pending[like_pending_n].like = like;
		like_pending_n++;
		like_pending_save();
	}
	native_log("LIKE: %s %s (cola %d)\n", like ? "me gusta" : "ya no",
	           uri, like_pending_n);
}

// The art worker: fetches and decodes covers off the render loop. The hero
// goes first -- it is what the user is looking at; the row thumbnails queue
// behind it. Pixels land in staging buffers and the render thread uploads
// them, because textures belong to the GPU context's thread.
static volatile char hero_want[200];
static volatile bool hero_new = false;
static char hero_loaded[200] = "";

static void art_thread(void *arg)
{
	(void)arg;
	static bool profile_done = false;   /* one avatar attempt, then never again */
	u64 like_try_at = 0;
	while (running) {
		/* Queued likes ride out when the rate-limited Web API has a window;
		 * same Retry-After discipline as the avatar fetch below. A 403 means
		 * the token predates the library-modify scope: stop and say so. */
		if (add_req) {
			char err[128] = "";
			char msg[96];
			if (native_playlist_add(add_req_playlist, add_req_track, err,
			                        sizeof(err)) == 0)
				snprintf(msg, sizeof(msg), "%s %s", tr(S_PL_ADDED), add_req_name);
			else
				snprintf(msg, sizeof(msg), "%s: %s", tr(S_PL_FAILED), err);
			toast_show(msg);
			__sync_synchronize();
			add_req = false;
			continue;
		}
		/* Likes and list thumbnails are extras: while the playing song is
		 * still downloading they wait, so they cannot starve it. */
		const bool stream_busy = native_stream_busy();
		if (like_pending_n && !like_scope_missing && !stream_busy) {
			const u64 now = osGetTime();
			if (!like_try_at ||
			    now - like_try_at >= (u64)(like_wait_s > 0 ? like_wait_s : 5) * 1000) {
				like_try_at = now;
				/* Back off on every failure, to ten minutes: the shared
				 * client id sits behind HTTP 429 for hours at a time. */
				like_wait_s = like_wait_s < 20 ? 20
				              : (like_wait_s * 2 > 600 ? 600 : like_wait_s * 2);
				const like_op op = like_pending[0];
				char lerr[128] = "";
				int ra = 0;
				const int rc = native_like(op.uri, op.like, lerr,
				                           sizeof(lerr), &ra);
				if (rc == 0) {
					like_wait_s = 0;   /* back to quick turns */
					memmove(&like_pending[0], &like_pending[1],
					        (size_t)(like_pending_n - 1) * sizeof(like_pending[0]));
					like_pending_n--;
					like_pending_save();
					native_log("LIKE: aplicado %s\n", op.uri);
				} else if (rc == 1) {
					like_scope_missing = true;
					native_log("LIKE: falta el scope user-library-modify; "
					           "desvincula en ajustes y vincula de nuevo\n");
				} else {
					if (ra > 0)
						like_wait_s = ra + 2;
					native_log("LIKE: sigue pendiente: %s\n", lerr);
				}
			}
		}

		/* The avatar gets exactly one shot: a local profile.png / profile.jpg
		 * beside the app wins; otherwise a single /v1/me call. The Web API
		 * rate-limits that endpoint so hard (HTTP 429 for hours on this shared
		 * client id) that retrying only burned TLS handshakes the whole
		 * session -- a failure is accepted and the hero stays the placeholder.
		 * The hero is only taken while nothing has played yet: once track art
		 * owns the screen, the avatar no longer displaces it. */
		if (!profile_done && !hero_loaded[0] && !hero_new) {
			profile_done = true;

			char dir[320], pfile[352], ferr[64] = "";
			cfg_dir(dir, sizeof(dir));
			snprintf(pfile, sizeof(pfile), "%sprofile.png", dir);
			bool got = artwork_load_file(pfile, ART_PIXELS, art_staging,
			                             ferr, sizeof(ferr));
			if (!got) {
				snprintf(pfile, sizeof(pfile), "%sprofile.jpg", dir);
				got = artwork_load_file(pfile, ART_PIXELS, art_staging,
				                        ferr, sizeof(ferr));
			}
			if (got) {
				snprintf(hero_loaded, sizeof(hero_loaded), "%s", pfile);
				__sync_synchronize();
				art_pending = true;
				native_log("INIT: avatar local %s\n", pfile);
				continue;
			}

			const char *part = native_profile_art();
			if (!part[0]) {
				native_profile_art_refresh();
				part = native_profile_art();
			}
			if (part[0]) {
				snprintf((char *)hero_want, sizeof(hero_want), "%s", part);
				__sync_synchronize();
				hero_new = true;
			}
		}
		if (hero_new) {
			char url[200];
			snprintf(url, sizeof(url), "%s", (const char *)hero_want);
			hero_new = false;
			char err[128] = "";
			if (url[0] &&
			    artwork_fetch_decode(url, ART_PIXELS, art_staging,
			                         err, sizeof(err))) {
				snprintf(hero_loaded, sizeof(hero_loaded), "%s", url);
				__sync_synchronize();
				art_pending = true;
			}
			continue;
		}
		if (lyr_want) {
			lyr_want = false;
			char err[160] = "";
			lyrics_result rc =
			    lyrics_fetch(lyr_w_track, lyr_w_artist, lyr_w_album,
			                 lyr_w_dur, &lyr_doc, err, sizeof(err));
			if (rc == LYR_ERR)
				snprintf(lyr_error, sizeof(lyr_error), "%s", err);
			__sync_synchronize();
			lyr_state = rc == LYR_OK ? LYR_ST_OK :
			            rc == LYR_NONE ? LYR_ST_NONE : LYR_ST_ERR;
			continue;
		}
		/* Drain a few thumbnails per cycle: one fetch per pass could not
		 * keep up with a fast fling, which read as flicker. */
		int drain;
		for (drain = 0; drain < 4 && !stream_busy && thumbs_work_pending();
		     drain++)
			thumbs_work_one();
		if (drain)
			continue;
		svcSleepThread(20000000LL);   // 20 ms
	}
}

// -------------------------------------------------------------------- config

// ------------------------------------------------------------------ drawing

#define TOP_W 400.0f
#define BOT_W 320.0f
#define SCR_H 240.0f

static C2D_TextBuf text_buf;
static C3D_Tex art_tex;
static Tex3DS_SubTexture art_sub = {ART_PIXELS, ART_PIXELS, 0.0f, 1.0f, 1.0f, 0.0f};

// Cuts a string to fit a width, with an ellipsis. Without this a long artist
// name simply runs past the edge of the screen and is clipped mid-letter.
static void text_fit(const char *str, float x, float y, float scale, u32 colour,
                     float max_w)
{
	if (!str || !*str) return;
	/* The top screen draws these twice a frame (one per eye) with strings
	 * that change once a song: fit each once and reuse it. Fitting by
	 * parsing into text_buf, as this used to, spent the frame's whole text
	 * arena on long titles and blanked the bottom screen's labels. */
	static struct {
		char src[192], fit[192];
		float scale, max_w;
	} memo[12];
	static int memo_next;
	const char *fit = NULL;
	for (int i = 0; i < (int)(sizeof(memo) / sizeof(memo[0])); i++)
		if (memo[i].scale == scale && memo[i].max_w == max_w &&
		    strcmp(memo[i].src, str) == 0) {
			fit = memo[i].fit;
			break;
		}
	if (!fit) {
		int i = memo_next;
		memo_next = (memo_next + 1) % (int)(sizeof(memo) / sizeof(memo[0]));
		snprintf(memo[i].src, sizeof(memo[i].src), "%s", str);
		snprintf(memo[i].fit, sizeof(memo[i].fit), "%s", str);
		ui_fit_scaled(text_buf, memo[i].fit, sizeof(memo[i].fit), scale, max_w);
		memo[i].scale = scale;
		memo[i].max_w = max_w;
		fit = memo[i].fit;
	}

	C2D_Text t;
	C2D_TextParse(&t, text_buf, fit);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, colour);
}

static void clock_text(char *out, size_t n, float secs);

static void text_at(const char *str, float x, float y, float scale, u32 colour, u32 align)
{
	if (!str || !*str) return;
	C2D_Text t;
	C2D_TextParse(&t, text_buf, str);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | align, x, y, 0.5f, scale, scale, colour);
}

// A square stand-in for the hundreds of tracks that carry no embedded cover.
static void draw_no_art(const Skin *sk, float x, float y, float size)
{
	C2D_DrawRectSolid(x, y, 0.0f, size, size, sk->panel);
	float pad = size * 0.28f;
	C2D_DrawRectSolid(x + pad, y + pad, 0.1f, size - pad * 2, size * 0.10f, sk->accent);
	C2D_DrawRectSolid(x + pad, y + size * 0.5f, 0.1f, size - pad * 2, size * 0.10f, sk->meter);
	C2D_DrawRectSolid(x + pad, y + size * 0.68f, 0.1f, (size - pad * 2) * 0.6f, size * 0.10f, sk->meter);
}

static void draw_art(const Skin *sk, float x, float y, float size)
{
	if (art_present) {
		C2D_Image img = {&art_tex, &art_sub};
		C2D_DrawImageAt(img, x, y, 0.1f, NULL, size / ART_PIXELS, size / ART_PIXELS);
	} else {
		draw_no_art(sk, x, y, size);
	}
}

// Album-art tint for the top screen background, the reference client's best
// trick: the cover's average colour, darkened hard, so the screen belongs to
// the record without fighting the text. tint_target 0 means "no art, use the
// skin"; the current colour glides toward the target a few percent a frame.
static u32 tint_cur = 0, tint_target = 0;

/* A flat mean over all pixels trends to muddy grey, because opposing hues
 * cancel. Weighting each pixel by its saturation lets the colours a person
 * would actually name dominate, while near-grey pixels contribute little;
 * very dark and blown-out pixels are skipped so black borders and white
 * backgrounds do not wash the result out. Darkened 45% at the end so white
 * text stays readable over it. */
static u32 tint_from_pixels(const u8 *rgb565)
{
	u32 acc_r = 0, acc_g = 0, acc_b = 0, acc_w = 0;
	for (int i = 0; i < ART_PIXELS * ART_PIXELS; i += 4) {
		u16 v;
		memcpy(&v, rgb565 + i * 2, 2);
		u32 r = ((v >> 11) & 31) * 255 / 31;
		u32 g = ((v >> 5) & 63) * 255 / 63;
		u32 b = (v & 31) * 255 / 31;
		u32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
		u32 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
		if (mx < 32 || mn > 232)
			continue;
		u32 w = mx - mn + 1;   /* saturation */
		acc_r += r * w;
		acc_g += g * w;
		acc_b += b * w;
		acc_w += w;
	}
	if (!acc_w)
		return C2D_Color32(18, 18, 19, 0xFF);   /* monochrome cover: slate */
	u32 r = acc_r / acc_w, g = acc_g / acc_w, b = acc_b / acc_w;
	return C2D_Color32(r * 45 / 100, g * 45 / 100, b * 45 / 100, 0xFF);
}

static u32 tint_step(u32 cur, u32 target)
{
	u32 out = 0xFF000000u;
	for (int i = 0; i < 3; i++) {
		int c = (cur >> (i * 8)) & 0xFF, t = (target >> (i * 8)) & 0xFF;
		c += (t - c) / 8 + ((t > c) ? 1 : (t < c) ? -1 : 0);
		if (c < 0) c = 0;
		if (c > 255) c = 255;
		out |= (u32)c << (i * 8);
	}
	return out;
}

// Stereo depth: each element shifts horizontally by its own amount, so the
// cover sits clearly in front of the text and the text in front of the panel.
// par is signed per eye and already scaled by the 3D slider.
static void draw_top(const Skin *sk, bool buffering, bool muted, float par)
{
	/* The reference client's wash: the cover's accent at full strength up
	 * top, easing to black at the hinge. The ramp shape comes from its
	 * mockup (stops at 0%, 46%, 78%, 100% of the height); the hue comes
	 * from the current art. Four-corner colours give true vertex
	 * interpolation, so three rects do the whole gradient. */
	static const struct { float pos, ratio; } stops[] = {
		{0.00f, 1.000f},
		{0.46f, 0.935f},
		{0.78f, 0.467f},
		{1.00f, 0.000f},
	};
	const u32 tr_ = tint_cur & 0xFF, tg = (tint_cur >> 8) & 0xFF,
	          tb_ = (tint_cur >> 16) & 0xFF;
	for (int i = 0; i + 1 < (int)(sizeof stops / sizeof stops[0]); i++) {
		const float y0 = stops[i].pos * 240.0f, y1 = stops[i + 1].pos * 240.0f;
		const u32 c0 = C2D_Color32((u8)(tr_ * stops[i].ratio),
		                           (u8)(tg * stops[i].ratio),
		                           (u8)(tb_ * stops[i].ratio), 0xFF);
		const u32 c1 = C2D_Color32((u8)(tr_ * stops[i + 1].ratio),
		                           (u8)(tg * stops[i + 1].ratio),
		                           (u8)(tb_ * stops[i + 1].ratio), 0xFF);
		C2D_DrawRectangle(0.0f, y0, 0.0f, TOP_W, y1 - y0, c0, c0, c1, c1);
	}

	/* Status line in what used to be an empty band: where the music comes
	 * from on the left; queue, battery and clock on the right. Flat (no
	 * parallax), like a system bar. */
	{
		const float sy = 7.0f;
		char right[48] = "";
		int q_done = 0, q_total = 0;
		native_dl_progress(&q_done, &q_total);
		if (q_total > 0)
			snprintf(right, sizeof(right), "DL %d/%d   ", q_done, q_total);
		if (batt_pct >= 0)
			snprintf(right + strlen(right), sizeof(right) - strlen(right),
			         "%s%d%%   ", batt_charging ? "+" : "", batt_pct);
		time_t now_t = time(NULL);
		struct tm *lt = localtime(&now_t);
		if (lt)
			snprintf(right + strlen(right), sizeof(right) - strlen(right),
			         "%02d:%02d", lt->tm_hour, lt->tm_min);
		text_at(right, TOP_W - 14.0f, sy, 0.44f,
		        batt_pct >= 0 && batt_pct <= 15 && !batt_charging ? sk->warm
		                                                          : sk->dim,
		        C2D_AlignRight);
		const char *ctx = play_idx >= 0 && play_ctx_title[0] ? play_ctx_title
		                                                     : "SpotiBacan";
		text_fit(ctx, 20.0f, sy, 0.44f, sk->dim, TOP_W - 20.0f - 150.0f);
	}

	// Art on the left with a real margin, text beside it, and nothing else.
	// The text column is measured, so a long title stops at the edge instead
	// of running off it. Its rows line up with the cover's top and bottom.
	const float ART = 160.0f;
	const float AX = 20.0f, AY = 30.0f;
	const float TX = AX + ART + 20.0f;
	const float TW = TOP_W - TX - 18.0f;

	draw_art(sk, AX + par * 3.0f, AY, ART);

	text_fit(now_title[0] ? now_title : tr(S_NOTHING_PLAYING), TX + par * 2.0f, AY,
	         0.70f, sk->text, TW);
	text_fit(now_artist, TX + par * 1.4f, AY + 30, 0.54f, sk->dim, TW);
	text_fit(now_album, TX + par * 1.0f, AY + 52, 0.46f, sk->dim, TW);

	const bool shown_paused = casting() ? cast_paused : paused;
	const char *state = buffering ? tr(S_STATE_LOADING) :
	                    shown_paused ? tr(S_STATE_PAUSED) :
	                    muted     ? tr(S_STATE_MUTED) : tr(S_STATE_PLAYING);
	C2D_DrawCircleSolid(TX + 5 + par * 0.8f, AY + 106, 0.1f, 4,
	                    buffering ? sk->warm : sk->accent);
	text_at(state, TX + 18 + par * 0.8f, AY + 97, 0.46f, sk->dim, 0);

	if (now_dur > 1.0f) {
		float done = now_pos / now_dur;
		if (done > 1.0f) done = 1.0f;
		float bx = TX + par * 0.6f, by = AY + ART - 22.0f, bw = TW;
		C2D_DrawRectSolid(bx, by, 0.0f, bw, 3, sk->meter);
		C2D_DrawRectSolid(bx, by, 0.1f, bw * done, 3, sk->accent);
		char a[12], b[12];
		clock_text(a, sizeof(a), now_pos);
		clock_text(b, sizeof(b), now_dur);
		text_at(a, bx, by + 8, 0.42f, sk->dim, 0);
		text_at(b, bx + bw, by + 8, 0.42f, sk->dim, C2D_AlignRight);
	}

	/* Up next: the strip under the art, free below the hinge line. Only
	 * shown when the next track is actually predictable -- shuffle picks at
	 * skip time and repeat-one replays this one. */
	if (!shuffle_on && repeat_mode != 2 && play_idx >= 0) {
		int n1 = play_idx + 1;
		if (n1 >= play_count)
			n1 = (repeat_mode == 1 && play_count > 0) ? 0 : -1;
		if (n1 >= 0) {
			float uy = 210.0f;
			ui_text_tracked(text_buf, tr(S_UPNEXT),
			                AX + par * 1.2f, ui_baseline(uy, TY_MICRO),
			                TY_MICRO, 1.0f, sk->meter);
			char line[120];
			snprintf(line, sizeof(line), "%s - %s", play_labels[n1],
			         play_subs[n1]);
			text_fit(line, AX + 86.0f + par * 1.2f, uy - 6.0f, 0.44f,
			         sk->dim, TOP_W - AX - 86.0f - 12.0f);
			int n2 = n1 + 1;
			if (n2 >= play_count)
				n2 = repeat_mode == 1 ? 0 : -1;
			if (n2 >= 0 && n2 != n1) {
				snprintf(line, sizeof(line), "%s - %s",
				         play_labels[n2], play_subs[n2]);
				text_fit(line, AX + 86.0f + par * 1.0f, uy + 8.0f,
				         0.40f, sk->dim,
				         TOP_W - AX - 86.0f - 12.0f);
			}
		}
	}

	// Last, above whatever the top screen shows: it only reports, and the
	// bottom screen keeps its controls uncovered.
	volume_overlay_args va = { text_buf, volume, vol_overlay };
	volume_overlay_draw(&va);
}

/* The visualizer's top screen: a dimmed wash of the cover colour, the
 * song on one line, the bars below. */
static void draw_vis_top(const Skin *sk, float par)
{
	const u32 tint = tint_cur ? tint_cur : sk->accent;
	const u32 wash = C2D_Color32((u8)((tint & 0xFF) / 5), (u8)(((tint >> 8) & 0xFF) / 5),
	                             (u8)(((tint >> 16) & 0xFF) / 5), 0xFF);
	C2D_DrawRectangle(0.0f, 0.0f, 0.0f, TOP_W, 240.0f, wash, wash,
	                  0xFF000000, 0xFF000000);
	char line[200];
	if (now_artist[0])
		snprintf(line, sizeof(line), "%s  -  %s", now_title, now_artist);
	else
		snprintf(line, sizeof(line), "%s",
		         now_title[0] ? now_title : tr(S_NOTHING_PLAYING));
	text_fit(line, 18.0f + par * 0.6f, 12.0f, 0.56f, sk->text, TOP_W - 36.0f);
	if (now_dur > 1.0f) {
		char a[12], b[12], t[32];
		clock_text(a, sizeof(a), now_pos);
		clock_text(b, sizeof(b), now_dur);
		snprintf(t, sizeof(t), "%s / %s", a, b);
		text_at(t, 18.0f + par * 0.6f, 30.0f, 0.42f, sk->dim, 0);
	}
	vis_draw(par, tint, vis_mode);
	volume_overlay_args va = { text_buf, volume, vol_overlay };
	volume_overlay_draw(&va);
}

#define BAND_H 40.0f          // title band across the top
#define BAR_Y  198.0f         // the full-width action bar (setup screens)

// Defined with the playback helpers below; the list needs them for the
// now-playing badge.
static bool now_matches_view(void);

static const C2D_Image *list_thumb(const char *url, void *ctx)
{
	(void)ctx;
	return thumbs_get(url);
}

// The visible window over the cached rows, rebuilt every frame: the list
// scrolls by pixels, so the window slides with it rather than in pages.
static void draw_native_list(const Skin *sk)
{
	// While the fetch thread is between views, the cached rows still belong
	// to the previous one. Drawing them under the new title reads as a
	// broken list, so the window stays empty until the cache matches what is
	// on screen. A fetch in progress is the exception: its chunks commit
	// progressively under the matching tag, and those partial rows do show.
	const char *want_arg = (nat_view == NATIVE_SEARCH) ? lib.query : nat_arg;
	bool match = nat_view == nat_loaded_view &&
	             strcmp(want_arg ? want_arg : "", nat_loaded_arg) == 0;
	bool loading = lib.loading || !match;
	bool show = match && lib.total > 0;

	int extra = (nat_view == NATIVE_PLAYLISTS) ? 2 : 0;
	int first = show ? (int)(lib.scroll / SL_ROW_H) : 0;
	float frac = show ? lib.scroll - first * SL_ROW_H : 0.0f;

	sl_row rows[8];
	int n = 0;
	if (show) {
		for (int i = first; i < lib.total && n < 7; i++) {
			sl_row *r = &rows[n++];
			if (extra && i == 0) {
				*r = (sl_row){tr(S_LIKED), "", "", true, false};
			} else if (extra && i == 1) {
				*r = (sl_row){tr(S_DL_VIEW), "", "", true, false};
			} else {
				const native_row *nr = &nat_rows[i - extra];
				*r = (sl_row){nr->label, nr->sub, nr->art, nr->is_playlist,
				              now_matches_view() && i - extra == now_idx &&
				              native_playback_state() == NATIVE_PLAYING};
			}
		}
	}

	char count[32] = "";
	if (show && lib.loading && fetch_total > nat_count)
		snprintf(count, sizeof(count), "%d / %d…", nat_count, fetch_total);
	else if (show)
		snprintf(count, sizeof(count), "%d / %d", lib.sel + 1, lib.total);

	screen_list_args a = {0};
	a.buf = text_buf;
	a.tb = &g_tb;
	a.title = nat_title;
	a.can_back = nat_view != NATIVE_PLAYLISTS;
	a.count = count[0] ? count : NULL;
	a.empty = loading ? tr(S_LOADING) :
	          nat_status[0] ? nat_status : tr(S_EMPTY_LIST);
	a.loading = loading && !show;
	a.rows = rows;
	a.n = n;
	a.first = first;
	a.frac = frac;
	a.total = show ? lib.total : 0;
	a.sel = lib.sel;
	a.pressed_id = g_touch.down ? g_touch.press_id : -1;
	a.animation_ms = (unsigned)osGetTime();
	a.thumb = list_thumb;
	a.accent = sk->accent;
	a.text = sk->text;
	a.dim = sk->dim;
	a.panel = sk->panel;
	a.meter = sk->meter;
	screen_list_draw(&a);
}


static void clock_text(char *out, size_t n, float secs)
{
	int t = (int)(secs < 0 ? 0 : secs);
	snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

// The settings menu: START opens it, and it is also the quit barrier Emilio
// asked for -- leaving the app now takes START, then "Salir", so a stray
// palm cannot kill the music. Rows draw up to z=0.1 and the list header at
// 0.5, so the panel lives at 0.6 and its type at 0.7. It takes over the
// touch map wholesale, so the view below cannot answer taps meant for it.
#define SETTINGS_ROWS 9
#define SET_ROW0      0x7100

/* Whether the list last shown holds tracks the queue can take. */
static bool list_downloadable(void)
{
	return nat_view != NATIVE_PLAYLISTS && nat_view != NATIVE_DOWNLOADS &&
	       nat_count > 0 && !native_offline();
}

/* Up/down for a long list: a press moves one row; held, it repeats after a
 * beat and picks up speed (five rows a step after three seconds), so a
 * 500-song list is not 500 presses. The circle pad scrolls too, faster the
 * further it is pushed. Returns the signed number of rows to move. */
typedef struct { u64 t0, next; } nav_hold;

static int nav_repeat(nav_hold *h, u32 down)
{
	const u64 now = osGetTime();
	const u32 held = hidKeysHeld();
	const int dir = (held & KEY_DUP) ? -1 : (held & KEY_DDOWN) ? +1 : 0;
	if (dir) {
		if (down & (KEY_DUP | KEY_DDOWN)) {
			h->t0 = now;
			h->next = now + 350;
			return dir;
		}
		if (now < h->next)
			return 0;
		const u64 held_ms = now - h->t0;
		h->next = now + (held_ms > 1500 ? 35 : 80);
		return dir * (held_ms > 3000 ? 5 : 1);
	}
	circlePosition cp;
	hidCircleRead(&cp);
	const int mag = cp.dy < 0 ? -cp.dy : cp.dy;   /* about 0..156 */
	if (mag < 40 || now < h->next)
		return 0;
	h->next = now + (mag > 140 ? 25 : mag > 100 ? 60 : 120);
	return (cp.dy > 0 ? -1 : +1) * (mag > 145 ? 3 : 1);
}

static int clamp_row(int sel, int total)
{
	if (sel >= total) sel = total - 1;
	return sel < 0 ? 0 : sel;
}

static bool casting(void);

/* START while a song plays: jump to its artist or album, or on to the
 * settings. Same modal pattern as the settings sheet, three rows. */
#define ACT_ROWS 9
#define ACT_ROW0 0x7200
static bool actions_open = false;
static int  actions_sel = 0;
/* Whose artist and album the menu offers, captured when it opens: the row
 * selected in an open list, else the song playing. */
static struct {
	char artist[128], artist_uri[40];
	char album[128], album_uri[40];
	bool has_row;       /* a track row was selected: it can be queued */
	native_row row;
} act;

/* Rows: 0 play next, 1 add to playlist, 2 artist, 3 album, 4 radio,
 * 5 visualizer, 6 delete download, 7 play on the speaker (or back),
 * 8 settings. Delete takes two presses:
 * the first only arms it. */
static bool act_delete_armed = false;
static bool unlink_armed = false;

/* The Spotify track the menu is about: the selected row, else the song
 * playing; "" for local files (they have no catalogue id to add). */
static const char *act_track_uri(void)
{
	const char *u = act.has_row ? act.row.uri : native_now_uri();
	return strncmp(u, "spotify:track:", 14) == 0 ? u : "";
}

static bool action_enabled(int row)
{
	if (row == 0) return act.has_row;
	if (row == 1) return act_track_uri()[0] != '\0' && !native_offline();
	if (row == 2) return act.artist_uri[0] != '\0';
	if (row == 3) return act.album_uri[0] != '\0';
	if (row == 4) return act_track_uri()[0] != '\0' && !native_offline();
	if (row == 5) return vis_open || native_now_uri()[0] != '\0';
	if (row == 6) return act.has_row && strncmp(act.row.uri, "file:", 5) == 0;
	if (row == 7) {
		if (casting())
			return true;
		if (native_offline() || native_cast_finding())
			return false;
		if (cast_find_row || native_cast_count() == 0)
			return true;   /* finding needs nothing playing */
		return native_now_uri()[0] != '\0';
	}
	return true;
}

/* Fills act; false when neither source has anything to go to. */
static bool actions_capture(void)
{
	memset(&act, 0, sizeof(act));
	if (lib.open) {
		const int extra = (nat_view == NATIVE_PLAYLISTS) ? 2 : 0;
		const int idx = lib.sel - extra;
		if (idx >= 0 && idx < nat_count && !nat_rows[idx].is_playlist &&
		    (strncmp(nat_rows[idx].uri, "spotify:track:", 14) == 0 ||
		     strncmp(nat_rows[idx].uri, "file:", 5) == 0)) {
			const native_row *r = &nat_rows[idx];
			act.has_row = true;
			act.row = *r;
			snprintf(act.artist, sizeof(act.artist), "%s", r->sub);
			snprintf(act.artist_uri, sizeof(act.artist_uri), "%s", r->artist_uri);
			snprintf(act.album, sizeof(act.album), "%s", r->album);
			snprintf(act.album_uri, sizeof(act.album_uri), "%s", r->album_uri);
			return true;
		}
	}
	snprintf(act.artist, sizeof(act.artist), "%s", native_now_artist());
	snprintf(act.artist_uri, sizeof(act.artist_uri), "%s", native_now_artist_uri());
	snprintf(act.album, sizeof(act.album), "%s", native_now_album());
	snprintf(act.album_uri, sizeof(act.album_uri), "%s", native_now_album_uri());
	/* Anything playing (a saved song too) at least offers the visualizer. */
	return act.artist_uri[0] || act.album_uri[0] || native_now_uri()[0];
}

static void draw_actions(const Skin *sk)
{
	const u32 dim   = C2D_Color32(0x00, 0x00, 0x00, 0xA8);
	const u32 bord  = C2D_Color32(0x38, 0x38, 0x38, 0xFF);
	const u32 panel = C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF);
	C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, BOT_W, SCR_H, dim);
	const float px = 20.0f, py = 4.0f, pw = 280.0f, ph = 232.0f;
	C2D_DrawRectSolid(px, py, 0.6f, pw, ph, bord);
	C2D_DrawRectSolid(px + 1.0f, py + 1.0f, 0.6f, pw - 2.0f, ph - 2.0f, panel);

	ui_depth(0.7f);
	char cast_label[80];
	const bool find_row = cast_find_row || native_cast_count() == 0;
	if (casting())
		snprintf(cast_label, sizeof(cast_label), "%s", tr(S_CAST_BACK));
	else if (native_cast_finding())
		snprintf(cast_label, sizeof(cast_label), "%s", tr(S_CAST_FINDING));
	else if (find_row)
		snprintf(cast_label, sizeof(cast_label), "%s", tr(S_CAST_FIND));
	else
		snprintf(cast_label, sizeof(cast_label), tr(S_CAST_TO),
		         native_cast_name());
	const char *labels[ACT_ROWS] = { tr(S_PLAY_NEXT), tr(S_ADD_TO_PL),
	                                 tr(S_GO_ARTIST), tr(S_GO_ALBUM),
	                                 tr(S_GO_RADIO),
	                                 tr(vis_open ? S_VIS_STOP : S_VIS_START),
	                                 tr(act_delete_armed ? S_DELETE_CONFIRM
	                                                     : S_DELETE_DL),
	                                 cast_label,
	                                 tr(S_SETTINGS) };
	/* Left/right on the row walks the speakers, then "Find speakers". */
	char cast_sub[64] = "";
	if (!casting() && find_row)
		snprintf(cast_sub, sizeof(cast_sub), "%s", tr(S_CAST_FIND_SUB));
	else if (!casting())
		snprintf(cast_sub, sizeof(cast_sub), tr(S_CAST_PICK),
		         native_cast_current() + 1, native_cast_count());
	const char *subs[ACT_ROWS] = { act.has_row ? act.row.label : "", "",
	                               act.artist, act.album, "", "", "",
	                               cast_sub, "" };
	/* Nine rows of 25px fill the sheet; a sub line sits right under. */
	const float rh = 25.0f, ry = py + 8.0f;
	for (int i = 0; i < ACT_ROWS; i++) {
		const float y = ry + i * rh;
		const bool on = action_enabled(i);
		if (i == actions_sel)
			C2D_DrawRectSolid(px + 6.0f, y - 3.0f, 0.65f, pw - 12.0f,
			                  rh - 2.0f, sk->accent);
		const u32 fg = i == actions_sel ? sk->on_accent
		               : (on ? sk->text : sk->dim);
		ui_text(text_buf, labels[i], px + 16.0f,
		        ui_baseline(y, TY_ROW_NAME), TY_ROW_NAME, pw - 32.0f, fg);
		if (subs[i][0])
			ui_text(text_buf, subs[i], px + 16.0f,
			        ui_baseline(y + 13.0f, TY_ROW_SUB), TY_ROW_SUB,
			        pw - 32.0f, i == actions_sel ? fg : sk->dim);
		tb_add(&g_tb, px + 6.0f, y - 3.0f, pw - 12.0f, rh - 2.0f,
		       ACT_ROW0 + i);
	}
	ui_depth(0.0f);
}

/* The playlist picker for "Add to playlist": a window of rows around the
 * selection over a dimmed screen. */
#define PICK_VIS 7
static void draw_pick(const Skin *sk)
{
	const u32 dim   = C2D_Color32(0x00, 0x00, 0x00, 0xA8);
	const u32 bord  = C2D_Color32(0x38, 0x38, 0x38, 0xFF);
	const u32 panel = C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF);
	C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, BOT_W, SCR_H, dim);
	const float px = 12.0f, py = 6.0f, pw = 296.0f, ph = 228.0f;
	C2D_DrawRectSolid(px, py, 0.6f, pw, ph, bord);
	C2D_DrawRectSolid(px + 1.0f, py + 1.0f, 0.6f, pw - 2.0f, ph - 2.0f, panel);
	ui_depth(0.7f);
	ui_text(text_buf, tr(S_ADD_TO_PL), px + 12.0f,
	        ui_baseline(py + 6.0f, TY_ROW_NAME), TY_ROW_NAME, pw - 24.0f, sk->warm);
	int first = pick_sel - PICK_VIS / 2;
	if (first > pick_n - PICK_VIS)
		first = pick_n - PICK_VIS;
	if (first < 0)
		first = 0;
	const float rh = 28.0f, ry = py + 30.0f;
	for (int i = 0; i < PICK_VIS && first + i < pick_n; i++) {
		const int idx = first + i;
		const float y = ry + i * rh;
		if (idx == pick_sel)
			C2D_DrawRectSolid(px + 6.0f, y - 3.0f, 0.65f, pw - 12.0f, rh - 2.0f,
			                  sk->accent);
		ui_text(text_buf, pick_rows[idx].label, px + 14.0f,
		        ui_baseline(y + 3.0f, TY_ROW_NAME), TY_ROW_NAME, pw - 28.0f,
		        idx == pick_sel ? sk->on_accent : sk->text);
		tb_add(&g_tb, px + 6.0f, y - 3.0f, pw - 12.0f, rh - 2.0f, ACT_ROW0 + 0x40 + i);
	}
	ui_depth(0.0f);
}

static void draw_settings(const Skin *sk)
{
	const u32 dim   = C2D_Color32(0x00, 0x00, 0x00, 0xA8);
	const u32 bord  = C2D_Color32(0x38, 0x38, 0x38, 0xFF);
	const u32 panel = C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, BOT_W, SCR_H, dim);
	const float px = 20.0f, py = 10.0f, pw = 280.0f, ph = 222.0f;
	C2D_DrawRectSolid(px, py, 0.6f, pw, ph, bord);
	C2D_DrawRectSolid(px + 1.0f, py + 1.0f, 0.6f, pw - 2.0f, ph - 2.0f,
	                  panel);

	ui_depth(0.7f);
	ui_text(text_buf, tr(S_SETTINGS), px + 16.0f,
	        ui_baseline(py + 8.0f, TY_ROW_NAME), TY_ROW_NAME, pw - 32.0f,
	        sk->warm);
	{
		/* Battery and build, top right: "87%  v1.3", "+" while charging. */
		char hdr[32] = "";
		if (batt_pct >= 0) {
			snprintf(hdr, sizeof(hdr), "%s%d%%", batt_charging ? "+" : "",
			         batt_pct);
			if (!batt_charging && batt_left_min >= 0)
				snprintf(hdr + strlen(hdr), sizeof(hdr) - strlen(hdr),
				         " ~%dh %02dm", batt_left_min / 60, batt_left_min % 60);
		}
#ifdef APP_VERSION
		snprintf(hdr + strlen(hdr), sizeof(hdr) - strlen(hdr), "%sv%s",
		         hdr[0] ? "   " : "", APP_VERSION);
#endif
		if (hdr[0]) {
			const u32 col = batt_pct >= 0 && batt_pct <= 15 && !batt_charging
			                ? sk->warm : sk->dim;   /* low: warn */
			float vw = ui_text_width(text_buf, hdr, TY_ROW_SUB);
			ui_text(text_buf, hdr, px + pw - 16.0f - vw,
			        ui_baseline(py + 10.0f, TY_ROW_SUB), TY_ROW_SUB, vw, col);
		}
	}
	if (settings_pending >= 0 || osGetTime() < settings_busy_until) {
		// Three animated dots while the change applies and a beat after.
		char dots[4] = "...";
		dots[(osGetTime() / 200) % 3 + 1] = '\0';
		float tw = ui_text_width(text_buf, tr(S_SETTINGS), TY_ROW_NAME);
		ui_text(text_buf, dots, px + 28.0f + tw,
		        ui_baseline(py + 8.0f, TY_ROW_NAME), TY_ROW_NAME, 32.0f,
		        sk->accent);
	}

	const char *labels[SETTINGS_ROWS] = {
		tr(S_LANGUAGE), tr(S_ACCENT), tr(S_DOWNLOADS), tr(S_QUALITY),
		tr(S_EQ), tr(S_DL_LIST), tr(S_UNLINK), tr(S_QUIT), tr(S_BACK_MENU),
	};
	const float rh = 22.0f, ry = py + 26.0f;
	for (int i = 0; i < SETTINGS_ROWS; i++) {
		const float y = ry + i * rh;
		if (i == settings_sel)
			C2D_DrawRectSolid(px + 6.0f, y - 3.0f, 0.65f, pw - 12.0f,
			                  rh - 2.0f, sk->accent);
		const u32 fg = i == settings_sel ? sk->on_accent : sk->text;
		ui_text(text_buf, labels[i], px + 16.0f,
		        ui_baseline(y + 2.0f, TY_ROW_NAME), TY_ROW_NAME,
		        pw - 100.0f, fg);
		// The preference rows show their current value on the right.
		if (i == 0) {
			const char *v = lang_get() == LANG_EN ? "English" : "Espanol";
			float w = ui_text_width(text_buf, v, TY_ROW_SUB);
			ui_text(text_buf, v, px + pw - 16.0f - w,
			        ui_baseline(y + 5.0f, TY_ROW_SUB), TY_ROW_SUB, w, fg);
		} else if (i == 1) {
			float w = ui_text_width(text_buf, tr(ACCENT_NAMES[accent_idx]),
			                        TY_ROW_SUB);
			C2D_DrawRectSolid(px + pw - 16.0f - w - 18.0f, y + 5.0f,
			                  0.65f, 12.0f, 12.0f,
			                  ACCENTS[accent_idx]);
			ui_text(text_buf, tr(ACCENT_NAMES[accent_idx]),
			        px + pw - 16.0f - w,
			        ui_baseline(y + 5.0f, TY_ROW_SUB), TY_ROW_SUB, w, fg);
		} else if (i == 2) {
			const char *v = downloads_on ? "ON" : "OFF";
			float w = ui_text_width(text_buf, v, TY_ROW_SUB);
			ui_text(text_buf, v, px + pw - 16.0f - w,
			        ui_baseline(y + 5.0f, TY_ROW_SUB), TY_ROW_SUB, w,
			        downloads_on ? fg : sk->dim);
		} else if (i == 3) {
			const char *v = sp_high_quality() ? "320 kbps" : "160 kbps";
			float w = ui_text_width(text_buf, v, TY_ROW_SUB);
			ui_text(text_buf, v, px + pw - 16.0f - w,
			        ui_baseline(y + 5.0f, TY_ROW_SUB), TY_ROW_SUB, w,
			        sp_high_quality() ? fg : sk->dim);
		} else if (i == 4) {
			static const int EQ_NAMES[EQ_COUNT] = {
				S_EQ_OFF, S_EQ_BASS, S_EQ_TREBLE, S_EQ_VOCAL, S_EQ_BOOST };
			const char *v = tr(EQ_NAMES[eq_get()]);
			float w = ui_text_width(text_buf, v, TY_ROW_SUB);
			ui_text(text_buf, v, px + pw - 16.0f - w,
			        ui_baseline(y + 5.0f, TY_ROW_SUB), TY_ROW_SUB, w,
			        eq_get() != EQ_OFF ? fg : sk->dim);
		} else if (i == 5) {
			/* Which list it would take, or why it cannot. */
			char note[96];
			if (list_downloadable())
				snprintf(note, sizeof(note), "%s (%d)", nat_title,
				         nat_count);
			else
				snprintf(note, sizeof(note), "%s", tr(S_DL_LIST_NOTE));
			ui_text(text_buf, note, px + 16.0f,
			        ui_baseline(y + 13.0f, TY_MICRO), TY_MICRO,
			        pw - 32.0f, i == settings_sel ? fg : sk->dim);
		} else if (i == 6) {
			ui_text(text_buf, tr(unlink_armed ? S_UNLINK_CONFIRM
			                                  : S_UNLINKED_NOTE), px + 16.0f,
			        ui_baseline(y + 14.0f, TY_MICRO), TY_MICRO,
			        pw - 32.0f, i == settings_sel ? fg : sk->dim);
		}
		tb_add(&g_tb, px + 6.0f, y - 3.0f, pw - 12.0f, rh - 2.0f,
		       SET_ROW0 + i);
	}
	ui_depth(0.0f);
}


// Recently-played shelf on the player screen, kept locally: the Web API's
// recently-played endpoint sits behind the same saturated quota as /v1/me,
// while what this console played is something it already knows.
#define RECENT_MAX 8
typedef struct { char uri[64]; char art[80]; } recent_t;
static recent_t recents[RECENT_MAX];
static int recent_count = 0;

static void recents_file(char *out, size_t n)
{
	char d[320];
	cfg_dir(d, sizeof(d));
	snprintf(out, n, "%srecents.txt", d);
}

static void recents_load(void)
{
	char path[352];
	recents_file(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[160];
	while (recent_count < RECENT_MAX && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = '\0';
		char *nl = strpbrk(tab + 1, "\r\n");
		if (nl)
			*nl = '\0';
		snprintf(recents[recent_count].uri, 64, "%s", line);
		snprintf(recents[recent_count].art, 80, "%s", tab + 1);
		recent_count++;
	}
	fclose(f);
}

static void recents_save(void)
{
	char path[352], tmp[360];
	recents_file(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < recent_count; i++)
		fprintf(f, "%s\t%s\n", recents[i].uri, recents[i].art);
	fclose(f);
	rename(tmp, path);
}

static void recents_push(const char *uri, const char *art)
{
	if (!uri || !*uri)
		return;
	int at = -1;
	for (int i = 0; i < recent_count; i++)
		if (strcmp(recents[i].uri, uri) == 0) {
			at = i;
			break;
		}
	if (at == 0)
		return;   // already the freshest
	if (at < 0) {
		if (recent_count < RECENT_MAX)
			recent_count++;
		at = recent_count - 1;
	}
	memmove(&recents[1], &recents[0], (size_t)at * sizeof(recents[0]));
	snprintf(recents[0].uri, sizeof(recents[0].uri), "%s", uri);
	snprintf(recents[0].art, sizeof(recents[0].art), "%s", art ? art : "");
	recents_save();
}

// Recent searches, kept locally for the long-press-on-the-magnifier popup:
// the reference client's search shelf, fed by what was actually typed here.
#define SRCH_MAX 8
static char srch[SRCH_MAX][64];
static int  srch_count = 0;
static bool srch_open = false;
#define SR_ROW0  0x7300
#define SR_DEL0  0x7320
#define SR_CLEAR 0x7340

static void srch_file(char *out, size_t n)
{
	char d[320];
	cfg_dir(d, sizeof(d));
	snprintf(out, n, "%ssearches.txt", d);
}

static void srch_load(void)
{
	char path[352];
	srch_file(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[80];
	while (srch_count < SRCH_MAX && fgets(line, sizeof(line), f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = '\0';
		if (!line[0])
			continue;
		snprintf(srch[srch_count], sizeof(srch[0]), "%s", line);
		srch_count++;
	}
	fclose(f);
}

static void srch_save(void)
{
	char path[352], tmp[360];
	srch_file(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < srch_count; i++)
		fprintf(f, "%s\n", srch[i]);
	fclose(f);
	rename(tmp, path);
}

static void srch_push(const char *query)
{
	if (!query || !*query)
		return;
	int at = -1;
	for (int i = 0; i < srch_count; i++)
		if (strcmp(srch[i], query) == 0) {
			at = i;
			break;
		}
	if (at == 0)
		return;
	if (at < 0) {
		if (srch_count < SRCH_MAX)
			srch_count++;
		at = srch_count - 1;
	}
	memmove(&srch[1], &srch[0], (size_t)at * sizeof(srch[0]));
	snprintf(srch[0], sizeof(srch[0]), "%s", query);
	srch_save();
}

// The popup: a dimmed modal over the list, same recipe as the settings menu.
static void draw_search_recents(const Skin *sk)
{
	const u32 dim   = C2D_Color32(0x00, 0x00, 0x00, 0xA8);
	const u32 bord  = C2D_Color32(0x38, 0x38, 0x38, 0xFF);
	const u32 panel = C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, BOT_W, SCR_H, dim);
	const float px = 30.0f, py = 30.0f, pw = 260.0f;
	const float rh = 26.0f;
	const float ph = 44.0f + rh * (srch_count > 0 ? srch_count : 1) + 10.0f;
	C2D_DrawRectSolid(px, py, 0.6f, pw, ph, bord);
	C2D_DrawRectSolid(px + 1.0f, py + 1.0f, 0.6f, pw - 2.0f, ph - 2.0f,
	                  panel);

	ui_depth(0.7f);
	ui_text(text_buf, tr(S_RECENT_TITLE), px + 14.0f,
	        ui_baseline(py + 8.0f, TY_ROW_NAME), TY_ROW_NAME, 140.0f,
	        sk->warm);
	if (srch_count > 0) {
		float w = ui_text_width(text_buf, tr(S_CLEAR), TY_MICRO);
		ui_text_tracked(text_buf, tr(S_CLEAR), px + pw - 14.0f - w,
		                ui_baseline(py + 12.0f, TY_MICRO), TY_MICRO, 1.0f,
		                sk->accent);
		tb_add(&g_tb, px + pw - 20.0f - w, py + 4.0f, w + 12.0f, 28.0f,
		       SR_CLEAR);
	}

	const float ry = py + 40.0f;
	if (srch_count == 0) {
		ui_text(text_buf, tr(S_EMPTY_LIST), px + 14.0f,
		        ui_baseline(ry + 4.0f, TY_ROW_SUB), TY_ROW_SUB,
		        pw - 28.0f, sk->dim);
	}
	for (int i = 0; i < srch_count; i++) {
		const float y = ry + i * rh;
		ui_text(text_buf, srch[i], px + 14.0f,
		        ui_baseline(y + 4.0f, TY_ROW_NAME), TY_ROW_NAME,
		        pw - 60.0f, sk->text);
		/* A small X to forget one search. */
		ui_text(text_buf, "x", px + pw - 24.0f,
		        ui_baseline(y + 5.0f, TY_ROW_NAME), TY_ROW_NAME, 12.0f,
		        sk->dim);
		tb_add(&g_tb, px + 4.0f, y, pw - 44.0f, rh, SR_ROW0 + i);
		tb_add(&g_tb, px + pw - 40.0f, y, 36.0f, rh, SR_DEL0 + i);
	}
	ui_depth(0.0f);
}

// Help sheet on SELECT: the controls for the view the user is standing in,
// plus the global ones. Single translated strings, so a button rename in one
// language cannot drift from its description.
static bool help_open = false;

static void draw_help(const Skin *sk)
{
	static const str_id general[] = { S_H_START, S_H_SELECT, S_H_LID };
	static const str_id list[] = {
		S_H_DPAD, S_H_LR_PAGE, S_H_A_OPEN, S_H_X_RADIO, S_H_SEARCH,
		S_H_HOLD_SEARCH, S_H_DRAG_LIST,
	};
	static const str_id player[] = {
		S_H_A_PAUSE, S_H_Y_MUTE, S_H_SKIP, S_H_SCRUB, S_H_MODES,
		S_H_HEART, S_H_SHELF, S_H_LETRA,
	};
	static const str_id lyrics[] = { S_H_B_BACK, S_H_DRAG_LYR };

	str_id sec;
	const str_id *rows;
	int n;
	if (lyr_open) {
		sec = S_H_SEC_LYRICS;
		rows = lyrics;
		n = (int)(sizeof(lyrics) / sizeof(lyrics[0]));
	} else if (lib.open) {
		sec = S_H_SEC_LIST;
		rows = list;
		n = (int)(sizeof(list) / sizeof(list[0]));
	} else {
		sec = S_H_SEC_PLAYER;
		rows = player;
		n = (int)(sizeof(player) / sizeof(player[0]));
	}
	const int ng = (int)(sizeof(general) / sizeof(general[0]));

	const u32 dim   = C2D_Color32(0x00, 0x00, 0x00, 0xA8);
	const u32 bord  = C2D_Color32(0x38, 0x38, 0x38, 0xFF);
	const u32 panel = C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, BOT_W, SCR_H, dim);
	const float px = 14.0f, py = 8.0f, pw = 292.0f, ph = 224.0f;
	C2D_DrawRectSolid(px, py, 0.6f, pw, ph, bord);
	C2D_DrawRectSolid(px + 1.0f, py + 1.0f, 0.6f, pw - 2.0f, ph - 2.0f,
	                  panel);

	ui_depth(0.7f);
	float y = py + 8.0f;
	ui_text(text_buf, tr(S_HELP_TITLE), px + 12.0f,
	        ui_baseline(y, TY_ROW_NAME), TY_ROW_NAME, pw - 24.0f, sk->warm);
	y += 22.0f;

	ui_text_tracked(text_buf, tr(sec), px + 12.0f,
	                ui_baseline(y, TY_MICRO), TY_MICRO, 1.0f, sk->accent);
	y += 12.0f;
	for (int i = 0; i < n; i++) {
		ui_text(text_buf, tr(rows[i]), px + 18.0f,
		        ui_baseline(y, TY_ROW_SUB), TY_ROW_SUB, pw - 36.0f,
		        sk->text);
		y += 14.5f;
	}

	y += 6.0f;
	ui_text_tracked(text_buf, tr(S_H_SEC_GENERAL), px + 12.0f,
	                ui_baseline(y, TY_MICRO), TY_MICRO, 1.0f, sk->accent);
	y += 12.0f;
	for (int i = 0; i < ng; i++) {
		ui_text(text_buf, tr(general[i]), px + 18.0f,
		        ui_baseline(y, TY_ROW_SUB), TY_ROW_SUB, pw - 36.0f,
		        sk->dim);
		y += 14.5f;
	}
	ui_depth(0.0f);
}

static bool queue_open = false;

/* The download queue popup: what is downloading right now, with a progress
 * bar, and what is still waiting its turn. Refreshes itself every frame --
 * the popup is read-only, so there is nothing to scroll or select. */
static void draw_queue(const Skin *sk)
{
	const u32 dim   = C2D_Color32(0x00, 0x00, 0x00, 0xA8);
	const u32 bord  = C2D_Color32(0x38, 0x38, 0x38, 0xFF);
	const u32 panel = C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.6f, BOT_W, SCR_H, dim);
	const float px = 14.0f, py = 8.0f, pw = 292.0f, ph = 224.0f;
	C2D_DrawRectSolid(px, py, 0.6f, pw, ph, bord);
	C2D_DrawRectSolid(px + 1.0f, py + 1.0f, 0.6f, pw - 2.0f, ph - 2.0f,
	                  panel);

	ui_depth(0.7f);
	float y = py + 8.0f;
	ui_text(text_buf, tr(S_QUEUE), px + 12.0f,
	        ui_baseline(y, TY_ROW_NAME), TY_ROW_NAME, pw - 24.0f, sk->warm);
	int q_done = 0, q_total = 0;
	native_dl_progress(&q_done, &q_total);
	if (q_total > 0) {
		char prog[32];
		snprintf(prog, sizeof(prog), "%d / %d", q_done, q_total);
		float w = ui_text_width(text_buf, prog, TY_ROW_SUB);
		ui_text(text_buf, prog, px + pw - 12.0f - w,
		        ui_baseline(y + 3.0f, TY_ROW_SUB), TY_ROW_SUB, w, sk->accent);
	}
	y += 24.0f;

	char active[128];
	const int pct = native_dl_active(active, sizeof(active));
	if (pct >= 0) {
		const bool saving = pct == NATIVE_DL_SAVING;
		ui_text_tracked(text_buf, tr(saving ? S_QUEUE_SAVING : S_QUEUE_DLING),
		                px + 12.0f,
		                ui_baseline(y, TY_MICRO), TY_MICRO, 1.0f,
		                sk->accent);
		y += 13.0f;
		ui_text(text_buf, active, px + 12.0f,
		        ui_baseline(y, TY_ROW_SUB), TY_ROW_SUB, pw - 24.0f,
		        sk->text);
		y += 13.0f;
		/* The bar: track underneath, accent fill on top. */
		C2D_DrawRectSolid(px + 12.0f, y, 0.7f, pw - 24.0f, 4.0f,
		                  C2D_Color32(0x4D, 0x4D, 0x4D, 0xFF));
		C2D_DrawRectSolid(px + 12.0f, y, 0.7f,
		                  (pw - 24.0f) * (float)(saving ? 100 : pct) / 100.0f, 4.0f,
		                  sk->accent);
		char pc[12];
		snprintf(pc, sizeof(pc), "%d%%", saving ? 100 : pct);
		ui_text(text_buf, pc, px + pw - 16.0f, ui_baseline(y + 7.0f,
		        TY_MICRO), TY_MICRO, 40.0f, sk->dim);
		y += 20.0f;
	}

	const int n = native_dl_queue_count();
	if (pct < 0 && n > 0) {
		/* Entries waiting but nothing running: the queue holds off while
		 * the playing track still downloads, so say so. */
		ui_text_tracked(text_buf, tr(S_QUEUE_WAITING), px + 12.0f,
		                ui_baseline(y, TY_MICRO), TY_MICRO, 1.0f, sk->dim);
		y += 16.0f;
	}
	if (pct < 0 && n == 0) {
		ui_text(text_buf, tr(S_QUEUE_EMPTY), px + 12.0f,
		        ui_baseline(y, TY_ROW_SUB), TY_ROW_SUB, pw - 24.0f,
		        sk->dim);
	} else {
		for (int i = 0; i < n && y < py + ph - 30.0f; i++) {
			char title[128];
			if (!native_dl_queue_entry(i, title, sizeof(title)))
				break;
			char num[140];
			snprintf(num, sizeof(num), "%d. %s", i + 1, title);
			ui_text(text_buf, num, px + 12.0f,
			        ui_baseline(y, TY_ROW_SUB), TY_ROW_SUB, pw - 24.0f,
			        sk->dim);
			y += 14.5f;
		}
		if (n > 0)
			ui_text(text_buf, tr(S_QUEUE_CLEAR), px + 12.0f,
			        ui_baseline(py + ph - 18.0f, TY_MICRO), TY_MICRO,
			        pw - 24.0f, sk->accent);
	}
	ui_depth(0.0f);
}

static void draw_bottom(const Skin *sk)
{
	if (lyr_open) {
		screen_lyrics_args a = {0};
		a.buf = text_buf;
		a.tb = &g_tb;
		a.track = now_title[0] ? now_title : tr(S_NOTHING_PLAYING);
		a.state = (screen_lyrics_state)lyr_state;
		a.error = lyr_error;
		a.doc = &lyr_doc;
		a.current = lyr_state == LYR_ST_OK
		            ? lyrics_index_at(&lyr_doc,
		                              (uint32_t)(now_pos * 1000.0f))
		            : -1;
		a.scroll = lyr_scroll;
		a.position_ms = (long)(now_pos * 1000.0f);
		a.pressed_id = g_touch.down ? g_touch.press_id : -1;
		a.animation_ms = (unsigned)osGetTime();
		a.accent = sk->accent;
		a.text = sk->text;
		a.dim = sk->dim;
		a.panel = sk->panel;
		a.meter = sk->meter;
		screen_lyrics_draw(&a);
	} else if (lib.open) {
		draw_native_list(sk);
	} else {
		// The player owns the whole bottom screen. Its scrubber only
		// reports: the backend streams a bounded fetch and there is
		// nothing to seek inside yet.
		native_state st = native_playback_state();

		screen_player_args a = {0};
		a.buf = text_buf;
		a.tb = &g_tb;
		a.track = now_title[0] ? now_title : tr(S_NOTHING_PLAYING);
		a.artist = now_artist;
		a.album = now_album;
		if (casting()) {   /* the button flips at the press, not the reply */
			a.playing = !cast_paused;
			a.loading = native_cast_state() == NATIVE_CAST_STARTING;
		} else {
			a.playing = st == NATIVE_PLAYING && !paused;
			a.loading = st == NATIVE_LOADING;
		}
		a.muted = muted;
		a.shuffle = shuffle_on;
		a.repeat = repeat_mode;
		a.liked = uri_is_liked(native_now_uri());
		a.dl_show = now_title[0] != '\0' &&
		            strncmp(native_now_uri(), "file:", 5) != 0 &&
		            !native_offline();
		a.dl_saved = native_current_saved();
		a.dl_armed = !a.dl_saved && (native_save_armed() || downloads_on);
		a.dl_queue = native_dl_queue_count();
		char tmp_title[128];
		a.dl_active = native_dl_active(tmp_title, sizeof(tmp_title)) >= 0;
		a.can_prev = play_idx > 0;
		a.can_next = play_idx >= 0 && play_idx + 1 < play_count;
		a.progress_ms = (long)(now_pos * 1000.0f);
		a.duration_ms = (long)(now_dur * 1000.0f);
		a.pressed_id = g_touch.down ? g_touch.press_id : -1;
		a.animation_ms = (unsigned)osGetTime();
		static const char *shelf_art[RECENT_MAX];
		for (int i = 0; i < recent_count; i++)
			shelf_art[i] = recents[i].art;
		a.recent_art = shelf_art;
		a.recent_n = recent_count;
		a.thumb = list_thumb;
		a.accent = sk->accent;
		a.on_accent = sk->on_accent;
		a.panel = sk->panel;
		screen_player_draw(&a);
	}
	text_at(BUILD_STAMP, BOT_W - 6, SCR_H - 14, 0.40f, sk->meter, C2D_AlignRight);

	if (settings_open) {
		// Modal: it replaces the touch map so the view below cannot answer
		// taps meant for the menu.
		tb_reset(&g_tb);
		draw_settings(sk);
	} else if (pick_open) {
		tb_reset(&g_tb);
		draw_pick(sk);
	} else if (actions_open) {
		tb_reset(&g_tb);
		draw_actions(sk);
	} else if (srch_open) {
		tb_reset(&g_tb);
		draw_search_recents(sk);
	} else if (help_open) {
		tb_reset(&g_tb);
		draw_help(sk);
	} else if (queue_open) {
		tb_reset(&g_tb);
		draw_queue(sk);
	}
	/* A short note over everything, e.g. "Added to Road trip". */
	if (toast[0] && osGetTime() < toast_until) {
		ui_depth(0.9f);
		const float w = BOT_W - 40.0f;
		C2D_DrawRectSolid(20.0f, SCR_H - 34.0f, 0.9f, w, 26.0f,
		                  C2D_Color32(0x10, 0x10, 0x10, 0xEE));
		ui_text(text_buf, toast, 30.0f, ui_baseline(SCR_H - 30.0f, TY_ROW_SUB),
		        TY_ROW_SUB, w - 20.0f, sk->text);
		ui_depth(0.0f);
	}
}

/* Set while track_ended starts the next song: the ring still holds the
 * finished song's last seconds, which must play out instead of being cut. */
static bool auto_advance = false;
/* Seconds of the old song left in the ring when the next one is fetched:
 * about what a track takes to start, so the handover is near gapless. */
#define END_LEAD_S 4
/* The decoder finished a song; the ring is still draining it. */
static bool end_pending = false;

/* "Play next" inserts into the snapshot right after the playing row, in
 * the order picked: queued_after counts how many already wait there.
 * play_edited says the snapshot no longer mirrors the list on screen, so
 * row indexes stop meaning on-screen highlights. */
static int  queued_after = 0;
static bool play_edited = false;

static void play_step(int idx);

/* Casting: the speaker in cast.txt plays and the console is its remote.
 * The console's own player stays paused where it was, so going back is
 * seamless unless something else was picked meanwhile. */
static bool cast_dirty = false;     /* a new pick to send to the speaker */
static bool cast_changed = false;   /* the speaker moved off the paused song */
static bool cast_paused = false;
static long cast_pos_ms = 0;        /* where the first song starts */
static char cast_uri[192] = "";     /* the song picked while casting */
static bool cast_was_on = false;    /* this cast got going at least once */
static u64  cast_lost_at = 0;       /* it dropped then; A reconnects */
#define CAST_LOST_MS 30000

/* A pressed while the "lost the speaker" prompt is up. */
static bool cast_lost_retry(void)
{
	if (!cast_lost_at || native_cast_state() != NATIVE_CAST_FAILED ||
	    osGetTime() - cast_lost_at > CAST_LOST_MS)
		return false;
	cast_lost_at = 0;
	cast_paused = false;
	native_cast_reconnect();
	return true;
}

static bool casting(void)
{
	const native_cast_status st = native_cast_state();
	return st == NATIVE_CAST_STARTING || st == NATIVE_CAST_ON;
}

/* The console keeps quiet: casting, or offering to reconnect. */
static bool cast_holds(void)
{
	return casting() || cast_lost_at;
}

// Starts a track by URI alone, for the shelf: no list context to remember,
// so prev/next grey out until a list play gives them bounds again.
static void play_uri(const char *uri)
{
	end_pending = false;
	queued_after = 0;
	play_edited = false;
	if (casting()) {
		snprintf(cast_uri, sizeof(cast_uri), "%s", uri);
		cast_dirty = true;
		cast_changed = true;
		cast_pos_ms = 0;
	} else {
		native_play(uri);
	}
	now_idx = -1;
	play_count = 0;   /* off-list: no neighbours to step to */
	play_idx = -1;
	buffering = !casting();
	paused = casting();
	ndspChnSetPaused(0, paused);
	if (!auto_advance)
		ring_clear();
}

// Starts a track from the cached rows and snapshots the whole list: that
// snapshot -- not whatever list is on screen -- is what prev/next, L/R and
// auto-advance walk, so the transport keeps working while the user browses
// somewhere else.
static void play_index(int idx)
{
	play_uri(nat_rows[idx].uri);
	/* The shelf is for Spotify art; a local file is one tap away in
	 * Downloads and its long path would not fit the recent entry anyway. */
	if (strncmp(nat_rows[idx].uri, "file:", 5) != 0)
		recents_push(nat_rows[idx].uri, nat_rows[idx].art);
	/* Only tracks go into the snapshot: an artist view mixes in release
	 * rows, which open rather than play. */
	int n = 0, at = -1;
	for (int i = 0; i < nat_count; i++) {
		if (nat_rows[i].is_playlist)
			continue;
		if (i == idx)
			at = n;
		snprintf(play_uris[n], sizeof(play_uris[n]), "%s", nat_rows[i].uri);
		snprintf(play_art[n], sizeof(play_art[n]), "%s", nat_rows[i].art);
		snprintf(play_labels[n], sizeof(play_labels[n]), "%s",
		         nat_rows[i].label);
		snprintf(play_subs[n], sizeof(play_subs[n]), "%s", nat_rows[i].sub);
		n++;
	}
	play_count = n;
	play_idx = at;
	now_idx = idx;
	/* Skipped rows shift snapshot indexes off the on-screen ones: later
	 * steps must not highlight by index. */
	play_edited = n != nat_count;
	snprintf(play_ctx_title, sizeof(play_ctx_title), "%s", nat_title);
	now_view = nat_view;
	snprintf(now_arg, sizeof(now_arg), "%s",
	         nat_view == NATIVE_SEARCH ? lib.query : nat_arg);
}

/* Sends what the console would play now to the speaker: the list it came
 * from as the context (so the speaker goes on through it by itself), from
 * that song. A saved file goes alone; the cast thread looks up its id. */
static bool cast_start(void)
{
	const char *track = (play_idx >= 0 && play_idx < play_count)
	                    ? play_uris[play_idx]
	                    : (cast_uri[0] ? cast_uri : native_now_uri());
	if (strncmp(track, "spotify:track:", 14) != 0 &&
	    strncmp(track, "file:", 5) != 0) {
		char msg[96];
		snprintf(msg, sizeof(msg), tr(S_CAST_LOCAL), native_cast_name());
		toast_show(msg);
		if (casting())
			native_cast_end();
		return false;
	}
	char ctx[192];
	if (play_idx >= 0 && (strncmp(now_arg, "spotify:playlist:", 17) == 0 ||
	                      strncmp(now_arg, "spotify:album:", 14) == 0))
		snprintf(ctx, sizeof(ctx), "%s", now_arg);
	else if (play_idx >= 0 && now_view == NATIVE_LIKED)
		snprintf(ctx, sizeof(ctx), "spotify:user:%s:collection",
		         native_account());
	else
		snprintf(ctx, sizeof(ctx), "%s", track);
	native_cast_play(ctx, strcmp(ctx, track) == 0 ? "" : track, cast_pos_ms);
	cast_paused = false;
	return true;
}

/* START -> "Play on <speaker>": the console pauses and the speaker takes
 * over from the same spot. */
static void cast_begin(void)
{
	cast_changed = false;
	cast_was_on = false;
	cast_lost_at = 0;
	cast_uri[0] = '\0';
	cast_pos_ms = (long)(now_pos * 1000.0f);
	if (cast_start()) {
		/* A song still loading would start under the speaker: drop it,
		 * coming back replays it from the list. */
		if (native_playback_state() != NATIVE_PLAYING) {
			native_stop();
			cast_changed = true;
		}
		paused = true;
		ndspChnSetPaused(0, true);
	}
}

/* START -> "Play on this console": the speaker stops; the console carries
 * on from its paused spot, or starts what was picked while casting. */
static void cast_finish(void)
{
	cast_was_on = false;
	cast_lost_at = 0;
	native_cast_end();
	if (!cast_changed && native_playback_state() == NATIVE_PLAYING) {
		paused = false;
		ndspChnSetPaused(0, false);
	} else if (play_idx >= 0 && play_idx < play_count) {
		play_step(play_idx);
	} else if (cast_uri[0]) {
		play_uri(cast_uri);
	}
}

// Plays a snapshot row (a manual or automatic skip). The snapshot stays the
// context; the on-screen highlight keeps pointing at the origin view, where
// the row index still matches if that list is the one displayed.
static void play_step(int idx)
{
	end_pending = false;
	/* Walking into the queued songs uses them up one by one; jumping
	 * anywhere else forgets the queue position. */
	if (idx == play_idx + 1 && queued_after > 0)
		queued_after--;
	else if (idx != play_idx)
		queued_after = 0;
	if (strncmp(play_uris[idx], "file:", 5) != 0)
		recents_push(play_uris[idx], play_art[idx]);
	if (casting()) {
		cast_dirty = true;
		cast_changed = true;
		cast_pos_ms = 0;
	} else {
		native_play(play_uris[idx]);
	}
	play_idx = idx;
	now_idx = play_edited ? -1 : idx;
	buffering = !casting();
	paused = casting();
	ndspChnSetPaused(0, paused);
	if (!auto_advance)
		ring_clear();
}

/* Puts a row right after the playing song (after anything already queued
 * that way). With nothing playing it simply plays the row. */
static void play_insert_next(const native_row *r)
{
	const char *cur = native_now_uri();
	if (play_idx < 0 || play_count <= 0) {
		if (!cur[0]) {
			play_uri(r->uri);
			return;
		}
		/* Off-list (the shelf): give the playing song a context of one. */
		snprintf(play_uris[0], sizeof(play_uris[0]), "%s", cur);
		snprintf(play_art[0], sizeof(play_art[0]), "%s", native_now_art());
		snprintf(play_labels[0], sizeof(play_labels[0]), "%s", now_title);
		snprintf(play_subs[0], sizeof(play_subs[0]), "%s", now_artist);
		play_count = 1;
		play_idx = 0;
		queued_after = 0;
	}
	if (play_count >= NATIVE_ROWS)
		play_count = NATIVE_ROWS - 1;   /* the list's tail gives way */
	int at = play_idx + 1 + queued_after;
	if (at > play_count)
		at = play_count;
	const int tail = play_count - at;
	memmove(play_uris[at + 1], play_uris[at], (size_t)tail * sizeof(play_uris[0]));
	memmove(play_art[at + 1], play_art[at], (size_t)tail * sizeof(play_art[0]));
	memmove(play_labels[at + 1], play_labels[at], (size_t)tail * sizeof(play_labels[0]));
	memmove(play_subs[at + 1], play_subs[at], (size_t)tail * sizeof(play_subs[0]));
	snprintf(play_uris[at], sizeof(play_uris[at]), "%s", r->uri);
	snprintf(play_art[at], sizeof(play_art[at]), "%s", r->art);
	snprintf(play_labels[at], sizeof(play_labels[at]), "%s", r->label);
	snprintf(play_subs[at], sizeof(play_subs[at]), "%s", r->sub);
	play_count++;
	queued_after++;
	play_edited = true;
	now_idx = -1;   /* list rows and snapshot rows no longer line up */
}

// Whether the list still shows the view the playing track came from.
static bool now_matches_view(void)
{
	const char *arg = nat_view == NATIVE_SEARCH ? lib.query : nat_arg;
	return now_idx >= 0 && now_view == nat_view &&
	       strcmp(now_arg, arg ? arg : "") == 0;
}

/* The row a manual or automatic skip lands on. Shuffle picks any row but the
 * current one. The bounds come from the playback snapshot, so skipping works
 * from any screen, not just the list the track started in. */
static int step_index(int dir)
{
	if (play_count == 0 || play_idx < 0)
		return -1;
	/* What the user queued plays next, shuffle or not. */
	if (dir > 0 && queued_after > 0 && play_idx + 1 < play_count)
		return play_idx + 1;
	if (shuffle_on && play_count > 1) {
		int r;
		do {
			r = rand() % play_count;
		} while (r == play_idx);
		return r;
	}
	const int t = play_idx + dir;
	return (t >= 0 && t < play_count) ? t : -1;
}

/* A track that finished on its own: repeat-one replays it, shuffle jumps
 * anywhere, repeat-all wraps at the end, and otherwise the next row plays.
 * Anything played off-list (the shelf) simply stops. */
static void track_ended(void)
{
	auto_advance = true;
	if (repeat_mode == 2 && native_now_uri()[0]) {
		/* Replay in place, keeping the list (and any queue) around it. */
		if (play_idx >= 0 && play_idx < play_count)
			play_step(play_idx);
		else
			play_uri(native_now_uri());
		auto_advance = false;
		return;
	}
	int next = step_index(+1);
	if (next < 0 && repeat_mode == 1 && !shuffle_on && play_count > 0)
		next = 0;
	if (next >= 0)
		play_step(next);
	auto_advance = false;
}

// What activating an absolute list row means: the synthetic Liked Songs or
// Downloads rows, drilling into a playlist, or playing a track.
/* One step up: a saved playlist inside Downloads goes back to Downloads,
 * everything else to the playlists. */
static void lib_back(void)
{
	if (nat_view == NATIVE_DOWNLOADS && nat_arg[0]) {
		nat_arg[0] = '\0';
		snprintf(nat_title, sizeof(nat_title), "%s", tr(S_DL_VIEW));
	} else {
		nat_view = NATIVE_PLAYLISTS;
		nat_arg[0] = '\0';
		snprintf(nat_title, sizeof(nat_title), "%s", tr(S_LISTS));
	}
	lib.sel = 0;
	native_fetch_cancel();   /* drop the old view's in-flight fetch */
	lib.scroll = lib.velocity = 0.0f;
	lib_reload = true;
}

static void activate_row(int abs)
{
	int extra = (nat_view == NATIVE_PLAYLISTS) ? 2 : 0;
	int idx = abs - extra;
	if (extra && abs == 0) {
		nat_view = NATIVE_LIKED;
		nat_arg[0] = '\0';
		snprintf(nat_title, sizeof(nat_title), "%s", tr(S_LIKED));
	} else if (extra && abs == 1) {
		nat_view = NATIVE_DOWNLOADS;
		nat_arg[0] = '\0';
		snprintf(nat_title, sizeof(nat_title), "%s", tr(S_DL_VIEW));
	} else if (idx >= 0 && idx < nat_count && nat_rows[idx].is_playlist &&
	           strncmp(nat_rows[idx].uri, "dlist:", 6) == 0) {
		/* A saved playlist inside Downloads: the same view, narrowed. */
		snprintf(nat_arg, sizeof(nat_arg), "%s", nat_rows[idx].uri);
		snprintf(nat_title, sizeof(nat_title), "%s", nat_rows[idx].label);
		nat_view = NATIVE_DOWNLOADS;
	} else if (idx >= 0 && idx < nat_count && nat_rows[idx].is_playlist) {
		snprintf(nat_arg, sizeof(nat_arg), "%s", nat_rows[idx].uri);
		if (strncmp(nat_arg, "spotify:album:", 14) == 0 ||
		    strncmp(nat_arg, "spotify:artist:", 15) == 0)
			/* Release and artist rows already know their name. */
			snprintf(nat_title, sizeof(nat_title), "%s", nat_rows[idx].label);
		else if (native_playlist_name(nat_arg, nat_title, sizeof(nat_title)) != 0)
			snprintf(nat_title, sizeof(nat_title), "%s", tr(S_PLAYLIST_FALLBACK));
		nat_view = NATIVE_TRACKS;
	} else if (idx >= 0 && idx < nat_count) {
		play_index(idx);
		return;
	} else {
		return;
	}
	lib.sel = 0;
				native_fetch_cancel();   /* drop the old view's in-flight fetch */
	lib.scroll = lib.velocity = 0.0f;
	lib_reload = true;
}

// Opens the lyrics view, queueing a fetch when the track changed since the
// last document. The art worker does the round trip; the view shows its
// state until the answer lands.
static void request_lyrics(void)
{
	lyr_open = true;
	const char *uri = native_now_uri();
	if (!uri[0] || strcmp(uri, lyr_track_uri) == 0)
		return;
	snprintf(lyr_track_uri, sizeof(lyr_track_uri), "%s", uri);
	snprintf(lyr_w_track, sizeof(lyr_w_track), "%s", native_now_title());
	snprintf(lyr_w_artist, sizeof(lyr_w_artist), "%s", native_now_artist());
	snprintf(lyr_w_album, sizeof(lyr_w_album), "%s", native_now_album());
	lyr_w_dur = (long)(native_duration() * 1000.0f);
	lyr_scroll = 0.0f;
	lyr_state = LYR_ST_LOADING;
	__sync_synchronize();
	lyr_want = true;
}

static void fail_screen(const char *why)
{
	consoleInit(GFX_TOP, NULL);
	printf("\x1b[2J\x1b[2;2H%s\n\n  Press START to exit.\n", why);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
}

static unsigned ring_free_space(void)
{
	return RING_BYTES - 1 - ring_used();
}

static void ring_sink_write(const unsigned char *pcm, unsigned len)
{
	ring_write(pcm, len);
}

// Each step of the native sign-in takes a visible moment, so it says which.
static char setup_step[64] = "";
static char setup_code[16] = "";
static char setup_url[128] = "";
static char setup_error[192] = "";

/* The few failures a person can do something about, in their language;
 * anything else is shown as the backend reported it. */
static const char *friendly_error(const char *e)
{
	if (!e)
		return "";
	if (strncmp(e, TLS_ERR_CLOCK_PREFIX, strlen(TLS_ERR_CLOCK_PREFIX)) == 0)
		return tr(S_ERR_CLOCK);
	if (strncmp(e, NATIVE_ERR_KEY_BLOCKED, 11) == 0)
		return tr(S_ERR_KEY_BLOCKED);
	/* A refused audio key is an account/catalog answer, not a bug: the
	 * friendly line, with the hex code left in the log. */
	if (strstr(e, "no permite esta cancion"))
		return tr(S_PREMIUM_REQ);
	if (strstr(e, "DNS failed") || strstr(e, "connect()") ||
	    strstr(e, "Network init"))
		return tr(S_ERR_NETWORK);
	return e;
}

static C3D_RenderTarget *g_top, *g_bottom;

// One painted screen for everything that happens before the library exists:
// signing in, pairing, failing. Same palette and spacing as the rest.
static void draw_setup(void)
{
	const Skin *sk = &SKIN;
	C2D_TextBufClear(text_buf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

	C2D_TargetClear(g_top, sk->top_bg);
	C2D_SceneBegin(g_top);
	if (setup_error[0]) {
		text_at(tr(S_SIGNIN_FAIL), 28, 40, 0.80f, sk->warm, 0);
		/* Wrapped, not clipped: the useful half of these messages ("set
		 * the Date & Time") used to fall off the right edge. */
		const char *nice = friendly_error(setup_error);
		float bl = ui_text_wrapped(text_buf, nice, 28.0f,
		                           ui_baseline(80.0f, TY_ROW_NAME),
		                           TY_ROW_NAME, TOP_W - 56.0f, sk->text, 5,
		                           1.35f);
		if (nice != setup_error)
			ui_text_wrapped(text_buf, setup_error, 28.0f, bl + 22.0f,
			                TY_ROW_SUB, TOP_W - 56.0f, sk->dim, 3, 1.3f);
	} else if (setup_code[0]) {
		text_at(tr(S_PAIR_TITLE), 28, 36, 0.80f, sk->warm, 0);
		text_at(tr(S_PAIR_LINE1), 28, 84, 0.50f, sk->dim, 0);
		text_fit(setup_url, 28, 108, 0.52f, sk->accent, TOP_W - 56);
		text_at(tr(S_PAIR_LINE2), 28, 146, 0.50f,
		        sk->dim, 0);
	} else {
		text_at("SpotiBacan", 28, 40, 0.90f, sk->warm, 0);
		text_fit(setup_step, 28, 96, 0.52f, sk->dim, TOP_W - 56);
	}

	C2D_TargetClear(g_bottom, sk->bottom_bg);
	C2D_SceneBegin(g_bottom);
	if (setup_code[0]) {
		C2D_DrawRectSolid(0, 0, 0.0f, BOT_W, BAND_H, sk->panel);
		text_at(tr(S_CODE), 16, 12, 0.46f, sk->dim, 0);
		text_at(setup_code, BOT_W / 2, 74, 1.30f, sk->text, C2D_AlignCenter);
		C2D_DrawRectSolid(0, BAR_Y, 0.0f, BOT_W, SCR_H - BAR_Y, sk->accent);
		text_at(tr(S_B_CANCEL), BOT_W / 2, BAR_Y + 10, 0.54f, sk->on_accent,
		        C2D_AlignCenter);
	} else if (setup_error[0]) {
		/* The offline hint is the useful action here, so it gets the
		 * middle of the screen in accent, not a line clipped under the
		 * bar. */
		text_at(tr(S_X_RETRY), BOT_W / 2, 84, 0.56f, sk->accent,
		        C2D_AlignCenter);
		text_at(tr(S_OFFLINE_A), BOT_W / 2, 124, 0.56f, sk->accent,
		        C2D_AlignCenter);
		C2D_DrawRectSolid(0, BAR_Y, 0.0f, BOT_W, SCR_H - BAR_Y, sk->accent);
		text_at(tr(S_START_QUIT), BOT_W / 2, BAR_Y + 10, 0.54f, sk->on_accent,
		        C2D_AlignCenter);
	} else {
		text_fit(setup_step, 16, 100, 0.50f, sk->dim, BOT_W - 32);
	}
	C3D_FrameEnd(0);
}

static void native_progress(const char *step)
{
	snprintf(setup_step, sizeof(setup_step), "%s", step);
	draw_setup();
}

// The pairing code has to stay readable while the user walks to a phone, so
// it is printed once and the waiting happens under it.
static void pair_show_code(const char *code, const char *url)
{
	snprintf(setup_code, sizeof(setup_code), "%s", code);
	snprintf(setup_url, sizeof(setup_url), "%s", url);
	draw_setup();
}

static bool pair_poll_frame(int attempt)
{
	(void)attempt;
	hidScanInput();
	if (hidKeysDown() & KEY_B)
		return false;
	draw_setup();
	return aptMainLoop();
}

/* Sleep and HOME land in the log with their time, for "slow after the
 * lid opened"; waking also tells the backend its links are gone. */
static aptHookCookie apt_cookie;

static void apt_event(APT_HookType hook, void *param)
{
	(void)param;
	switch (hook) {
	case APTHOOK_ONSLEEP:   native_log("APT: sleep\n"); break;
	case APTHOOK_ONWAKEUP:  native_log("APT: wake\n"); native_on_wake(); break;
	case APTHOOK_ONSUSPEND: native_log("APT: HOME\n"); break;
	case APTHOOK_ONRESTORE: native_log("APT: back from HOME\n"); break;
	default: break;
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	/* Let the network workers borrow the system core. Half of it: the
	 * console's own network services run there too, and starving them
	 * would slow the very downloads this is meant to speed up. */
	APT_SetAppCpuTimeLimit(50);
	aptHook(&apt_cookie, apt_event, NULL);
	skin_init();
	// The token belongs beside the .3dsx, like every other app here; without
	// this it would land at the card root and not match the probes.
	cfg_set_path((argv && argv[0]) ? argv[0] : NULL);
	prefs_load();
	refresh_nat_title();
	recents_load();
	srch_load();
	liked_load();
	like_pending_load();
	native_set_downloads(downloads_on);
	native_set_progress_cb(list_progress);
	srand((unsigned)osGetTime());

	// Everything that can fail runs first, while a text console is still the
	// only thing initialised and can report why.
	const char *fail = NULL;
	u32 *soc_buffer = memalign(0x1000, SOC_BUFFER);
	if (!soc_buffer || socInit(soc_buffer, SOC_BUFFER) != 0)
		fail = "Network init failed.";

	LightLock_Init(&ring.lock);
	ring.data = malloc(RING_BYTES);
	art_staging = malloc(ART_BYTES);
	if (!fail && (!ring.data || !art_staging)) fail = "Out of memory.";

	const u32 buf_bytes = FRAMES_PER_BUF * 4;   // stereo, 16-bit
	u8 *audio = NULL;
	float mix[12] = {0};
	if (!fail) {
		ndspInit();
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
		ndspChnSetRate(0, (float)SAMPLE_RATE);
		ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
		mix[0] = mix[1] = 1.0f;
		ndspChnSetMix(0, mix);
		mcuHwcInit();   // the overlay follows the hardware volume slider
		ptmuInit();     // charging state for the battery readout
		audio = linearAlloc(buf_bytes * WAVE_BUFFERS);
		if (!audio) fail = "Audio memory failed.";
	}
	if (fail) { fail_screen(fail); gfxExit(); return 1; }

	memset(waves, 0, sizeof(waves));
	for (int i = 0; i < WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = audio + i * buf_bytes;
		waves[i].nsamples = FRAMES_PER_BUF;
		waves[i].status = NDSP_WBUF_DONE;
	}

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	// Up before signing in, so pairing and progress are drawn screens rather
	// than a text console.
	// Setup screens draw mono; stereo turns on for the main loop below.
	gfxSet3D(false);
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	text_buf = C2D_TextBufNew(4096);
	ui_init();
	thumbs_init();
	g_top = top;
	g_bottom = bottom;

	{
		static char nat_err[192];
		static const native_sink sink = { ring_sink_write, ring_free_space };
		snprintf(setup_step, sizeof(setup_step), "%s", tr(S_STARTING));
		draw_setup();

		// First run on a console: pair here rather than in a separate tool.
		bool ok = true;
	retry_signin:
		ok = true;
		setup_error[0] = '\0';
		if (!native_has_token())
			ok = native_pair(pair_show_code, pair_poll_frame, nat_err,
			                 sizeof(nat_err)) == 0;
		setup_code[0] = '\0';
		if (ok)
			ok = native_init(&sink, native_progress, nat_err,
			                 sizeof(nat_err)) == 0;
		if (!ok) {
			snprintf(setup_error, sizeof(setup_error), "%s", nat_err);
			/* No internet (or no account) should not brick the app: the
			 * downloads are local files, so A drops into an offline player
			 * that opens straight on them. */
			while (aptMainLoop()) {
				hidScanInput();
				u32 d = hidKeysDown();
				if (d & KEY_X) {
					/* A fixed clock or a found network needs no restart. */
					snprintf(setup_step, sizeof(setup_step), "%s",
					         tr(S_STARTING));
					goto retry_signin;
				}
				if (d & KEY_A) {
					ok = native_init_offline(&sink) == 0;
					if (ok) {
						nat_view = NATIVE_DOWNLOADS;
						refresh_nat_title();
					}
					break;
				}
				if (d & KEY_START) break;
				draw_setup();
			}
			if (!ok) {
				ui_exit();
				C2D_Fini();
				C3D_Fini();
				gfxExit();
				return 1;
			}
		}
	}
	C3D_TexInit(&art_tex, ART_PIXELS, ART_PIXELS, GPU_RGB565);
	C3D_TexSetFilter(&art_tex, GPU_LINEAR, GPU_LINEAR);

	// Three threads beside the render loop: one services library requests,
	// one moves the ring into NDSP, one fetches and decodes cover art.
	// The lister and art threads run TLS handshakes on these stacks, which
	// need real headroom (chunked metadata names a 500-song list as a
	// dozen sequential handshakes); the audio thread only copies bytes.
	/* Lists and covers are network work: off the app core with the
	 * downloads, so drawing and decoding keep it to themselves. They rank
	 * above the download workers (0x31), though: someone is waiting on a
	 * list, nobody on the queue, and at the bottom a list starved behind
	 * a queued song (a search named 20 songs in 6.4 s, 0.3 s idle). */
	Thread lister = native_spawn_worker(native_library_thread, NULL, 96 * 1024, 0x30);
	Thread snd = threadCreate(audio_thread, NULL, 16 * 1024, 0x18, -2, false);
	Thread art = native_spawn_worker(art_thread, NULL, 96 * 1024, 0x30);
	buffering = false;
	lib.open = true;
	lib_reload = true;
	snprintf(now_artist, sizeof(now_artist), "%s", native_account());
	/* The idle hero is the user's own avatar; the art thread fetches it (and
	 * retries it past the Web API rate limit) and hands it over as soon as it
	 * lands, while nothing has played yet. */
	int nat_state_seen = -1;

	// One second: the link measured comfortable at 1024 kbps, and every extra
	// second of cushion is a second of lag after a skip.
	prebuffer_bytes = (u32)SAMPLE_RATE * 4;

	/* Stereo for the main screens. Both eyes are cleared and drawn every
	 * frame -- the old bug was a right buffer that only got cleared with the
	 * slider up, so the hardware showed uninitialised memory. Parallax comes
	 * from the slider, scaled by each layer's depth inside draw_top. */
	gfxSet3D(true);


	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		bool open_search = false;

		// The rects belong to the frame just drawn, so this runs before any
		// of them are rebuilt below.
		touch_update(&g_touch, g_tb.rects, g_tb.n);
		/* B closes the list, then B on the player opens it again: judged
		 * against the state at the top of the frame, so one press never
		 * does both. */
		const bool lib_was_open = lib.open;

		// A settings change armed last frame applies now: the menu already
		// showed its busy hint, so the SD write stalls behind feedback
		// instead of looking like a freeze.
		if (settings_pending >= 0) {
			const int row = settings_pending;
			settings_pending = -1;
			if (row == 0) {
				lang_set(lang_get() == LANG_EN ? LANG_ES : LANG_EN);
				cfg_set("lang", lang_get() == LANG_EN ? "en" : "es");
				refresh_nat_title();
			} else if (row == 1) {
				accent_idx = (accent_idx + 1) % ACCENT_COUNT;
				apply_accent();
				cfg_set("accent", ACCENT_KEYS[accent_idx]);
			} else if (row == 2) {
				downloads_on = !downloads_on;
				native_set_downloads(downloads_on);
				cfg_set("downloads", downloads_on ? "1" : "0");
			} else if (row == 3) {
				/* Takes effect from the next track fetched. */
				sp_set_high_quality(!sp_high_quality());
				cfg_set("quality", sp_high_quality() ? "320" : "160");
			} else if (row == 4) {
				/* Heard at once: the filter sits after the ring. */
				eq_set((eq_preset)((eq_get() + 1) % EQ_COUNT));
				cfg_set("eq", eq_key(eq_get()));
			}
			settings_busy_until = osGetTime() + 700;
		}

		// START opens the settings menu, and the menu owns every key and
		// touch until it is answered. Quitting is a row in it, so leaving
		// the app takes two deliberate presses.
		if (settings_open) {
			if (down & KEY_DUP)
				settings_sel = settings_sel > 0 ? settings_sel - 1
				                                : SETTINGS_ROWS - 1;
			if (down & KEY_DDOWN)
				settings_sel = settings_sel + 1 < SETTINGS_ROWS
				               ? settings_sel + 1 : 0;
			int row = -1;
			if (down & KEY_A)
				row = settings_sel;
			if (g_touch.clicked >= SET_ROW0 &&
			    g_touch.clicked < SET_ROW0 + SETTINGS_ROWS)
				row = g_touch.clicked - SET_ROW0;
			if (settings_sel != 6 || (row >= 0 && row != 6))
				unlink_armed = false;   /* moving away disarms unlink */
			switch (row) {
			case 0:   // language -- applied next frame, after the busy hint
			case 1:   // accent colour -- same
			case 2:   // downloads toggle -- same
			case 3:   // audio quality -- same
			case 4:   // equalizer preset -- same
				settings_pending = row;
				break;
			case 5:   // queue every track of the list last shown
				if (list_downloadable()) {
					settings_open = false;
					if (native_dl_enqueue_list(nat_rows, nat_count,
					                           nat_title) > 0)
						queue_open = true;   /* show what got queued */
					else
						toast_show(tr(S_DL_ALL_SAVED));   /* nothing new */
				}
				break;
			case 6:   // unlink: wipe the token (and prefs) and leave
				/* Re-pairing needs a phone: ask once more, as delete does. */
				if (!unlink_armed) {
					unlink_armed = true;
					break;
				}
				cfg_clear();
				goto quit;
			case 7:   // quit
				goto quit;
			default:
				break;
			}
			if (row == SETTINGS_ROWS - 1 || (down & (KEY_B | KEY_START))) {
				settings_open = false;
				unlink_armed = false;
			}
			down = 0;
			g_touch.clicked = -1;
			g_touch.long_pressed = -1;
		} else if (pick_open) {
			static nav_hold pick_hold;
			const int step = nav_repeat(&pick_hold, down);
			if (step && pick_n)
				pick_sel = clamp_row(pick_sel + step, pick_n);
			if (down & KEY_L)
				pick_sel = pick_sel > PICK_VIS ? pick_sel - PICK_VIS : 0;
			if (down & KEY_R)
				pick_sel = pick_sel + PICK_VIS < pick_n ? pick_sel + PICK_VIS
				                                        : pick_n - 1;
			int chosen = -1;
			if (down & KEY_A)
				chosen = pick_sel;
			if (g_touch.clicked >= ACT_ROW0 + 0x40 &&
			    g_touch.clicked < ACT_ROW0 + 0x40 + PICK_VIS) {
				int first = pick_sel - PICK_VIS / 2;
				if (first > pick_n - PICK_VIS)
					first = pick_n - PICK_VIS;
				if (first < 0)
					first = 0;
				chosen = first + (g_touch.clicked - ACT_ROW0 - 0x40);
			}
			if (chosen >= 0 && chosen < pick_n && !add_req) {
				snprintf(add_req_playlist, sizeof(add_req_playlist), "%s",
				         pick_rows[chosen].uri);
				snprintf(add_req_name, sizeof(add_req_name), "%s",
				         pick_rows[chosen].label);
				snprintf(add_req_track, sizeof(add_req_track), "%s", pick_track);
				__sync_synchronize();
				add_req = true;
				toast_show(tr(S_PL_ADDING));
				pick_open = false;
			}
			if (down & (KEY_B | KEY_START))
				pick_open = false;
			down = 0;
			g_touch.clicked = -1;
			g_touch.long_pressed = -1;
		} else if (actions_open) {
			if (down & (KEY_DUP | KEY_DDOWN))
				act_delete_armed = false;   /* moving away disarms delete */
			if (down & KEY_DUP)
				actions_sel = actions_sel > 0 ? actions_sel - 1 : ACT_ROWS - 1;
			if (down & KEY_DDOWN)
				actions_sel = actions_sel + 1 < ACT_ROWS ? actions_sel + 1 : 0;
			const int ncast = native_cast_count();
			if (actions_sel == 7 && !casting() && ncast > 0 &&
			    !native_cast_finding() &&
			    (down & (KEY_DLEFT | KEY_DRIGHT))) {
				const int dir = (down & KEY_DRIGHT) ? +1 : -1;
				int pos = cast_find_row ? ncast : native_cast_current();
				pos = (pos + dir + ncast + 1) % (ncast + 1);
				cast_find_row = pos == ncast;
				if (!cast_find_row)
					native_cast_pick(pos);
			}
			int row = -1;
			if (down & KEY_A)
				row = actions_sel;
			if (g_touch.clicked >= ACT_ROW0 &&
			    g_touch.clicked < ACT_ROW0 + ACT_ROWS)
				row = g_touch.clicked - ACT_ROW0;
			if (row == 6 && action_enabled(6) && !act_delete_armed) {
				act_delete_armed = true;   /* ask once more */
				row = -1;
			}
			if (row >= 0 && action_enabled(row)) {
				actions_open = false;
				if (row == 0) {
					play_insert_next(&act.row);
				} else if (row == 1) {
					snprintf(pick_track, sizeof(pick_track), "%s",
					         act_track_uri());
					pick_n = native_playlists_cached(pick_rows, NATIVE_ROWS);
					pick_sel = 0;
					if (pick_n > 0)
						pick_open = true;
					else
						toast_show(tr(S_PL_NONE));
				} else if (row == 4) {
					/* A radio seeded from the song, as X does in a list. */
					snprintf(nat_arg, sizeof(nat_arg), "%s", act_track_uri());
					nat_view = NATIVE_RADIO;
					snprintf(nat_title, sizeof(nat_title), "%s", tr(S_RADIO));
					lib.open = true;
					lib.sel = 0;
					native_fetch_cancel();
					lib.scroll = lib.velocity = 0.0f;
					lib_reload = true;
				} else if (row == 5) {
					vis_open = !vis_open;
					if (vis_open)
						lib.open = false;   /* the player stays below */
				} else if (row == 6) {
					act_delete_armed = false;
					native_delete_download(act.row.uri);
					lib_reload = true;   /* the Downloads list rereads the card */
					nat_loaded_view = (native_view)-1;
				} else if (row == 7) {
					if (casting()) {
						cast_finish();
					} else if (cast_find_row || native_cast_count() == 0) {
						native_cast_find();
						toast_show(tr(S_CAST_FINDING));
					} else {
						cast_begin();
					}
				} else if (row == 8) {
					settings_open = true;
					settings_sel = 0;
				} else {
					/* An album or artist opens like a playlist would. */
					nat_view = NATIVE_TRACKS;
					snprintf(nat_arg, sizeof(nat_arg), "%s",
					         row == 2 ? act.artist_uri : act.album_uri);
					snprintf(nat_title, sizeof(nat_title), "%s",
					         row == 2 ? act.artist : act.album);
					lib.open = true;
					lib.sel = 0;
					native_fetch_cancel();
					lib.scroll = lib.velocity = 0.0f;
					lib_reload = true;
				}
			}
			if (down & (KEY_B | KEY_START))
				actions_open = false;
			down = 0;
			g_touch.clicked = -1;
			g_touch.long_pressed = -1;
		} else if (srch_open) {
			// The recents popup: pick one, forget one, or wipe them all.
			const int c = g_touch.clicked;
			if (c >= SR_ROW0 && c < SR_ROW0 + srch_count) {
				snprintf(lib.query, sizeof(lib.query), "%s",
				         srch[c - SR_ROW0]);
				srch_push(lib.query);
				nat_view = NATIVE_SEARCH;
				snprintf(nat_title, sizeof(nat_title), "%s",
				         tr(S_SEARCH_VIEW));
				lib.sel = 0;
				native_fetch_cancel();   /* drop the old view's in-flight fetch */
				lib.scroll = lib.velocity = 0.0f;
				lib_reload = true;
				srch_open = false;
			} else if (c >= SR_DEL0 && c < SR_DEL0 + srch_count) {
				const int i = c - SR_DEL0;
				memmove(&srch[i], &srch[i + 1],
				        (size_t)(srch_count - i - 1) * sizeof(srch[0]));
				srch_count--;
				srch_save();
			} else if (c == SR_CLEAR) {
				srch_count = 0;
				srch_save();
			}
			if (down & (KEY_B | KEY_START))
				srch_open = false;
			down = 0;
			g_touch.clicked = -1;
			g_touch.long_pressed = -1;
		} else if (help_open) {
			// Any of the usual escape hatches closes the help sheet.
			if (down & (KEY_B | KEY_START | KEY_SELECT | KEY_A))
				help_open = false;
			down = 0;
			g_touch.clicked = -1;
			g_touch.long_pressed = -1;
		} else if (queue_open) {
			// Live: X empties what is still waiting, anything else closes.
			if (down & KEY_X)
				native_dl_clear();
			else if (down & (KEY_B | KEY_START | KEY_SELECT | KEY_A) ||
			         g_touch.clicked >= 0)
				queue_open = false;
			down = 0;
			g_touch.clicked = -1;
			g_touch.long_pressed = -1;
		} else if (down & KEY_SELECT) {
			help_open = true;
			down = 0;
		} else if (down & KEY_START) {
			/* With a Spotify track playing, START offers its artist and
			 * album first; otherwise it is straight to the settings. */
			if (actions_capture()) {
				actions_open = true;
				act_delete_armed = false;
				/* In Downloads the likely wish is the delete row. */
				actions_sel = strncmp(act.row.uri, "file:", 5) == 0 ? 6
				              : (act.has_row ? 0 : 5);
			} else {
				settings_open = true;
				settings_sel = 0;
			}
			down = 0;
		}

		// A scrub is a drag, not a tap: the position follows the finger while
		// held (a preview only -- the decoder keeps playing), and the seek
		// fires on release.
		static bool scrubbing = false;
		if (!lib.open && !lyr_open && now_dur > 1.0f) {
			if (g_touch.pressed && g_touch.press_id == BTN_P_SCRUB)
				scrubbing = true;
		} else {
			scrubbing = false;
		}
		if (scrubbing) {
			if (g_touch.down) {
				float frac =
				    (g_touch.px - SCRUB_BAR_X) / SCRUB_BAR_W;
				if (frac < 0.0f) frac = 0.0f;
				if (frac > 1.0f) frac = 1.0f;
				now_pos = frac * now_dur;
			} else {
				native_seek(now_pos);
				ring_clear();   /* the cushion holds pre-seek audio */
				scrubbing = false;
			}
		}

		// Player-mode touches fire on release.
		if (!lib.open && !lyr_open && g_touch.clicked >= 0) {
			switch (g_touch.clicked) {
			case BTN_P_PLAY:
				if (cast_lost_retry()) {
				} else if (casting()) {
					cast_paused = !cast_paused;
					native_cast_command(cast_paused ? "pause" : "resume");
				} else if (native_playback_state() == NATIVE_PLAYING) {
					paused = !paused;
					ndspChnSetPaused(0, paused);
				}
				break;
			case BTN_P_PREV: {
				if (casting()) {
					native_cast_command("skip_prev");
					cast_changed = true;
					break;
				}
				const int t = step_index(-1);
				if (t >= 0)
					play_step(t);
				break;
			}
			case BTN_P_NEXT: {
				if (casting()) {
					native_cast_command("skip_next");
					cast_changed = true;
					break;
				}
				const int t = step_index(+1);
				if (t >= 0)
					play_step(t);
				break;
			}
			case BTN_P_SHUFFLE:
				shuffle_on = !shuffle_on;
				break;
			case BTN_P_REPEAT:
				repeat_mode = (repeat_mode + 1) % 3;
				break;
			case BTN_P_LIKE:
				/* Local files have nothing to like on the server. */
				if (strncmp(native_now_uri(), "file:", 5) != 0)
					like_toggle(native_now_uri());
				break;
			case BTN_P_DOWNLOAD:
				native_save_current();
				break;
			case BTN_P_QUEUE:
				// Short press: the Downloads shelf itself.
				lib.open = true;
				nat_view = NATIVE_DOWNLOADS;
				nat_arg[0] = '\0';
				snprintf(nat_title, sizeof(nat_title), "%s",
				         tr(S_DL_VIEW));
				lib.sel = 0;
				native_fetch_cancel();   /* drop the old view's in-flight fetch */
				lib.scroll = lib.velocity = 0.0f;
				lib_reload = true;
				break;
			case BTN_P_LIBRARY:
				lib.open = true;
				lib_reload = true;
				break;
			case BTN_P_LYRICS:
				request_lyrics();
				break;
			case BTN_P_MUTE:
				muted = !muted;
				mix[0] = mix[1] = muted ? 0.0f : 1.0f;   // slider owns the level
				ndspChnSetMix(0, mix);
				break;
			}
			if (g_touch.clicked >= BTN_P_RECENT0 &&
			    g_touch.clicked < BTN_P_RECENT0 + recent_count) {
				// The push reshuffles the shelf, so copy before it moves.
				char uri[64], art[80];
				const recent_t *r = &recents[g_touch.clicked - BTN_P_RECENT0];
				snprintf(uri, sizeof(uri), "%s", r->uri);
				snprintf(art, sizeof(art), "%s", r->art);
				recents_push(uri, art);
				play_uri(uri);
			}
		}

		// Holding the queue button shows what is still downloading.
		if (!lib.open && !lyr_open &&
		    g_touch.long_pressed == BTN_P_QUEUE)
			queue_open = true;

		{
			// The player runs on its own thread; this just mirrors it.
			native_state st = native_playback_state();
			if (st != nat_state_seen) {
				const int prev_state = nat_state_seen;
				nat_state_seen = (int)st;
				/* The decoder is done, the speaker is not: wait until
				 * about as much audio is left as the next track takes to
				 * start, then fetch it, so neither end gets cut. */
				if (st == NATIVE_IDLE && prev_state == NATIVE_PLAYING)
					end_pending = true;
				switch (st) {
				case NATIVE_LOADING:
					snprintf(now_title, sizeof(now_title), "%s", tr(S_LOADING_DOTS));
					now_artist[0] = '\0';
					now_album[0] = '\0';
					break;
				case NATIVE_FAILED:
					snprintf(now_title, sizeof(now_title), "%s", tr(S_PLAY_FAIL));
					snprintf(now_artist, sizeof(now_artist), "%s",
					         friendly_error(native_last_error()));
					break;
				default:
					break;
				}
			}
			{
				static u64 busy_at;
				char dl_title[8];
				/* A closed lid must not sleep the remote either. */
				const bool busy = cast_holds() || st == NATIVE_PLAYING ||
				                  st == NATIVE_LOADING || end_pending ||
				                  ring_used() > 0 ||
				                  native_dl_queue_count() > 0 ||
				                  native_dl_active(dl_title, sizeof(dl_title)) >= 0;
				const u64 now_ms = osGetTime();
				if (busy) {
					busy_at = now_ms;
					lid_mode_set(true);
				} else if (now_ms - busy_at > LID_RELEASE_MS) {
					lid_mode_set(false);
				}
			}
			/* The speaker walks its own list while casting. */
			if (end_pending && !cast_holds() &&
			    ring_used() < (u32)SAMPLE_RATE * 4 * END_LEAD_S) {
				end_pending = false;
				track_ended();
			}
			if (st == NATIVE_PLAYING) {
				snprintf(now_title, sizeof(now_title), "%s", native_now_title());
				snprintf(now_artist, sizeof(now_artist), "%s", native_now_artist());
				snprintf(now_album, sizeof(now_album), "%s", native_now_album());
				if (!scrubbing) {
					/* The decoder runs a ring ahead of the speaker. */
					float queued = (float)ring_used() /
					               (float)(SAMPLE_RATE * 4);
					now_pos = native_position() - queued;
					if (now_pos < 0.0f)
						now_pos = 0.0f;
				}
				now_dur = native_duration();
				server_playing = true;
				// The cover follows the track; the art worker fetches it and
				// the placeholder shows until it lands.
				const char *art = native_now_art();
				if (!hero_new && strcmp(art, hero_loaded) != 0) {
					snprintf((char *)hero_want, sizeof(hero_want), "%s", art);
					__sync_synchronize();
					hero_new = true;
					art_present = false;
					tint_target = 0;   // back to the skin until the cover lands
				}
			} else if (paused && !cast_holds()) {
				// The track ended or failed under a paused channel; do not
				// carry the pause into whatever plays next. While casting
				// the pause is what keeps the console quiet.
				paused = false;
				ndspChnSetPaused(0, false);
			}
		}

		if (lib.open) {
			static nav_hold lib_hold;
			const int step = nav_repeat(&lib_hold, down);
			if (step && lib.total)
				lib.sel = clamp_row(lib.sel + step, lib.total);
			if (down & KEY_L) lib.sel = lib.sel >= 25 ? lib.sel - 25 : 0;
			if ((down & KEY_R) && lib.total)
				lib.sel = lib.sel + 25 < lib.total ? lib.sel + 25
				                                   : lib.total - 1;

			// Keep the d-pad selection inside the window, but only when it
			// just moved: applied unconditionally this clamp drags the
			// window back over any touch scrolling, anchoring the list to
			// whatever row the buttons last highlighted.
			static int prev_sel = -1;
			if (lib.total && lib.sel != prev_sel) {
				float top = lib.sel * SL_ROW_H, bottom = top + SL_ROW_H;
				if (top < lib.scroll) lib.scroll = top;
				else if (bottom > lib.scroll + SL_CONTENT_H)
					lib.scroll = bottom - SL_CONTENT_H;
			}
			prev_sel = lib.sel;

			// Drag 1:1 while held, then coast: retain a filtered portion of
			// the final motion and decay it after release. A fresh touch
			// always catches the list immediately.
			if (g_touch.pressed)
				lib.velocity = 0.0f;
			if (g_touch.down && g_touch.dragging) {
				float delta = -(float)g_touch.dy;
				lib.scroll += delta;
				lib.velocity = lib.velocity * 0.25f + delta * 0.75f;
				if (lib.velocity > SL_FLING_MAX) lib.velocity = SL_FLING_MAX;
				if (lib.velocity < -SL_FLING_MAX) lib.velocity = -SL_FLING_MAX;
			} else if (!g_touch.down) {
				lib.scroll += lib.velocity;
				lib.velocity *= SL_FLING_FRICTION;
				if (lib.velocity > -SL_FLING_STOP &&
				    lib.velocity < SL_FLING_STOP)
					lib.velocity = 0.0f;
			}
			float maxs = screen_list_max_scroll(lib.total);
			if (lib.scroll < 0.0f) { lib.scroll = 0.0f; lib.velocity = 0.0f; }
			if (lib.scroll > maxs) { lib.scroll = maxs; lib.velocity = 0.0f; }

			if (g_touch.long_pressed == SL_BTN_SEARCH)
				srch_open = true;
			if (g_touch.clicked == SL_BTN_SEARCH)
				open_search = true;
			else if (g_touch.clicked == SL_BTN_CLOSE)
				lib.open = false;
			else if (g_touch.clicked == SL_BTN_BACK &&
			         nat_view != NATIVE_PLAYLISTS) {
				// Same trip the B button makes.
				lib_back();
			}
			else if (g_touch.clicked >= SL_ROW0)
				activate_row(g_touch.clicked - SL_ROW0);

			if (down & KEY_A)
				activate_row(lib.sel);
			if (down & KEY_Y)
				open_search = true;
			if ((down & KEY_X) && lib.total && !native_offline()) {
				// X on a track seeds a radio from it (a network view).
				int extra = (nat_view == NATIVE_PLAYLISTS) ? 2 : 0;
				int idx = lib.sel - extra;
				if (idx >= 0 && idx < nat_count &&
				    !nat_rows[idx].is_playlist &&
				    strncmp(nat_rows[idx].uri, "file:", 5) != 0) {
					snprintf(nat_arg, sizeof(nat_arg), "%s",
					         nat_rows[idx].uri);
					nat_view = NATIVE_RADIO;
					snprintf(nat_title, sizeof(nat_title), "%s", tr(S_RADIO));
					lib.sel = 0;
				native_fetch_cancel();   /* drop the old view's in-flight fetch */
					lib.scroll = lib.velocity = 0.0f;
					lib_reload = true;
				}
			}
			if (down & KEY_B) {
				if (nat_view != NATIVE_PLAYLISTS) {
					lib_back();
				} else {
					lib.open = false;
				}
			}
		}

		if (vis_open && !lib_was_open && !lib.open && !lyr_open) {
			/* B leaves the visualizer; X flips between its two looks. */
			if (down & KEY_B) {
				vis_open = false;
				down &= ~KEY_B;
			}
			if (down & KEY_X) {
				vis_mode = (vis_style)((vis_mode + 1) % VIS_STYLE_COUNT);
				down &= ~KEY_X;
			}
		}
		if (!lib_was_open && !lib.open && !lyr_open && (down & KEY_B)) {
			lib.open = true;
			lib_reload = true;
		}

		if (!lib.open && (down & KEY_A)) {
			// A mirrors the on-screen disc: pause, not stop. The decoder
			// sleeps when the ring is full, so resume costs nothing.
			if (cast_lost_retry()) {
			} else if (casting()) {
				cast_paused = !cast_paused;
				native_cast_command(cast_paused ? "pause" : "resume");
			} else if (native_playback_state() == NATIVE_PLAYING) {
				paused = !paused;
				ndspChnSetPaused(0, paused);
			}
		}
		if (!lib.open && (down & KEY_Y)) {
			muted = !muted;
			mix[0] = mix[1] = muted ? 0.0f : 1.0f;   // slider owns the level
			ndspChnSetMix(0, mix);
		}

		// D-pad left/right are the physical prev/next; shuffle may reroute
		// them to a random row.
		// While casting the speaker walks its own copy of the list, so
		// the skips go to it; up/down step its volume (the slider only
		// reaches this console's speakers).
		if (casting() && !lib.open && !lyr_open) {
			if (down & (KEY_DLEFT | KEY_DRIGHT | KEY_L | KEY_R)) {
				native_cast_command((down & (KEY_DRIGHT | KEY_R))
				                    ? "skip_next" : "skip_prev");
				cast_changed = true;
			}
			if (down & (KEY_DUP | KEY_DDOWN)) {
				native_cast_volume_step((down & KEY_DUP) ? +1 : -1);
				char msg[96];
				int v = native_cast_volume() + ((down & KEY_DUP) ? 5 : -5);
				v = v < 0 ? 0 : v > 100 ? 100 : v;
				snprintf(msg, sizeof(msg), tr(S_CAST_VOL),
				         native_cast_name(), v);
				toast_show(msg);
			}
			down &= ~(KEY_DLEFT | KEY_DRIGHT | KEY_L | KEY_R |
			          KEY_DUP | KEY_DDOWN);
		}
		if (!lib.open && !lyr_open && (down & (KEY_DLEFT | KEY_DRIGHT))) {
			const int dir = (down & KEY_DRIGHT) ? +1 : -1;
			const int t = step_index(dir);
			if (t >= 0)
				play_step(t);
		}

		// L/R are the physical prev/next: with the lid closed they are the
		// only transport within reach. Volume lives on the hardware slider,
		// so these keys are free for the job that actually needs them.
		if (!lib.open && !lyr_open && (down & (KEY_L | KEY_R))) {
			const int dir = (down & KEY_R) ? +1 : -1;
			const int t = step_index(dir);
			if (t >= 0)
				play_step(t);
		}

		{
			/* A pick made while casting goes to the speaker; the state
			 * shows in the title lines, where the song would be. */
			static native_cast_status cast_seen = NATIVE_CAST_OFF;
			if (cast_dirty) {
				cast_dirty = false;
				if (casting())
					cast_start();
			}
			{
				/* A speaker search just ended: say what it found and land
				 * the row on a speaker again. */
				static bool finding_seen = false;
				const bool finding = native_cast_finding();
				if (finding_seen && !finding) {
					const int n = native_cast_found();
					char msg[96];
					if (n > 0)
						snprintf(msg, sizeof(msg), tr(S_CAST_FOUND), n);
					else
						snprintf(msg, sizeof(msg), "%s", tr(S_CAST_NONE));
					toast_show(msg);
					if (native_cast_count() > 0)
						cast_find_row = false;
				}
				finding_seen = finding;
			}
			if (cast_holds() && !paused) {   /* never both at once */
				paused = true;
				ndspChnSetPaused(0, true);
			}
			const native_cast_status cs = native_cast_state();
			if (cs != cast_seen) {
				char msg[160];
				if (cs == NATIVE_CAST_ON) {
					cast_was_on = true;
					snprintf(msg, sizeof(msg), tr(S_CAST_ON),
					         native_cast_name());
				} else if (cs == NATIVE_CAST_FAILED && cast_was_on) {
					/* It was playing: most likely a sleep or a lost
					 * link, and the speaker may well still be on. */
					cast_lost_at = osGetTime();
					snprintf(msg, sizeof(msg), tr(S_CAST_LOST),
					         native_cast_name());
				} else if (cs == NATIVE_CAST_FAILED)
					snprintf(msg, sizeof(msg), tr(S_CAST_FAILED),
					         native_cast_name());
				else
					msg[0] = '\0';
				if (msg[0])
					toast_show(msg);
				if (cs == NATIVE_CAST_FAILED)
					native_log("CAST: UI shows failure: %s\n",
					           native_cast_error());
				cast_seen = cs;
			}
			/* The offer stays up until taken or stale, so it is still
			 * there when the lid opens again. */
			if (cast_lost_at && cs == NATIVE_CAST_FAILED &&
			    osGetTime() - cast_lost_at <= CAST_LOST_MS) {
				char msg[160];
				snprintf(msg, sizeof(msg), tr(S_CAST_LOST),
				         native_cast_name());
				toast_show(msg);
			} else {
				cast_lost_at = 0;
			}
			if (cs == NATIVE_CAST_STARTING || cs == NATIVE_CAST_ON) {
				if (!cast_changed && play_idx >= 0 && play_idx < play_count) {
					snprintf(now_title, sizeof(now_title), "%s",
					         play_labels[play_idx]);
				} else if (cast_changed) {
					snprintf(now_title, sizeof(now_title), "%s",
					         play_ctx_title[0] ? play_ctx_title
					                           : native_cast_name());
				}
				snprintf(now_artist, sizeof(now_artist),
				         tr(cs == NATIVE_CAST_ON ? S_CAST_ON
				                                 : S_CAST_STARTING),
				         native_cast_name());
			}
		}
		{
			static u64 batt_at = 0;
			const u64 now_ms = osGetTime();
			if (!batt_at || now_ms - batt_at >= 5000) {
				batt_at = now_ms;
				u8 pct = 0, chg = 0;
				if (R_SUCCEEDED(MCUHWC_GetBatteryLevel(&pct)))
					batt_pct = pct > 100 ? 100 : pct;
				if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&chg)))
					batt_charging = chg != 0;
				/* The slope since a reference point: reset on the charger
				 * or a rise, and trusted only once the percent has fallen
				 * a little over a few minutes, so one tick cannot swing
				 * it wildly. */
				static int ref_pct = -1;
				static u64 ref_ms = 0;
				if (batt_pct >= 0) {
					if (batt_charging || ref_pct < 0 || batt_pct > ref_pct) {
						ref_pct = batt_pct;
						ref_ms = now_ms;
						batt_left_min = -1;
					} else {
						const int dropped = ref_pct - batt_pct;
						const u64 span = now_ms - ref_ms;
						if (dropped >= 2 && span >= 4 * 60 * 1000) {
							const double ms_per_pct = (double)span / dropped;
							batt_left_min = (int)(batt_pct * ms_per_pct / 60000.0);
						}
					}
				}
			}
		}

		// The overlay mirrors the hardware slider; any move pops it up.
		{
			u8 lvl;
			if (R_SUCCEEDED(MCUHWC_GetSoundSliderLevel(&lvl)) &&
			    lvl != vol_slider) {
				vol_slider = lvl;
				volume = lvl * 100 / 63;
				vol_overlay = 255;
				vol_hold = 90;
			}
		}
		if (vol_hold > 0)
			vol_hold--;
		else if (vol_overlay > 0)
			vol_overlay = vol_overlay > 4 ? vol_overlay - 4 : 0;

		// Lyrics mode: back, or dragging a plain document by hand.
		if (lyr_open) {
			if (g_touch.clicked == LYR_BTN_BACK || (down & KEY_B))
				lyr_open = false;
			if (lyr_state == LYR_ST_OK && !lyr_doc.synced &&
			    g_touch.down && g_touch.dragging) {
				lyr_scroll -= g_touch.dy;
				float max = screen_lyrics_max_scroll(lyr_doc.count);
				if (lyr_scroll < 0.0f) lyr_scroll = 0.0f;
				if (lyr_scroll > max) lyr_scroll = max;
			}
		}

		if (open_search) {
			// The system keyboard suspends this loop; audio keeps playing
			// because it lives on its own thread.
			SwkbdState kb;
			swkbdInit(&kb, SWKBD_TYPE_QWERTY, 2, sizeof(lib.query) - 1);
			swkbdSetHintText(&kb, tr(S_SEARCH_HINT));
			swkbdSetInitialText(&kb, lib.query);
			char typed[sizeof(lib.query)] = "";
			if (swkbdInputText(&kb, typed, sizeof(typed)) == SWKBD_BUTTON_RIGHT) {
				strncpy(lib.query, typed, sizeof(lib.query) - 1);
				if (lib.query[0])
					srch_push(lib.query);
				nat_view = lib.query[0] ? NATIVE_SEARCH : NATIVE_PLAYLISTS;
				snprintf(nat_title, sizeof(nat_title), "%s",
				         lib.query[0] ? tr(S_SEARCH_VIEW) : tr(S_LISTS));
				lib.sel = 0;
				native_fetch_cancel();   /* drop the old view's in-flight fetch */
				lib.scroll = lib.velocity = 0.0f;
				lib_reload = true;
			}
		}

		// GPU calls belong to this thread, so the fetchers only stage bytes.
		if (art_pending) {
			static uint8_t art_tiled[ART_BYTES];
			artwork_tile_rgb565(art_staging, art_tiled, ART_PIXELS);
			C3D_TexLoadImage(&art_tex, art_tiled, GPU_TEXFACE_2D, 0);
			C3D_TexFlush(&art_tex);
			tint_target = tint_from_pixels(art_staging);
			art_present = true;
			art_pending = false;
		}
		thumbs_pump();

		const Skin *sk = &SKIN;

		C2D_TextBufClear(text_buf);
		tb_reset(&g_tb);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		const float par = osGet3DSliderState() * 1.5f;
		tint_cur = tint_step(tint_cur, tint_target ? tint_target : sk->top_bg);
		if (vis_open) {
			/* The slot NDSP is playing and where in it: the bars follow
			 * the speaker, not the decoder running seconds ahead. */
			int slot = -1;
			if (!paused && !buffering)
				for (int i = 0; i < WAVE_BUFFERS; i++)
					if (waves[i].status == NDSP_WBUF_PLAYING)
						slot = i;
			vis_update(slot, ndspChnGetSamplePos(0));
		}
		C2D_TargetClear(top, 0xFF000000);
		C2D_SceneBegin(top);
		if (vis_open)
			draw_vis_top(sk, -par);
		else
			draw_top(sk, buffering, muted, -par);
		C2D_TargetClear(topR, 0xFF000000);
		C2D_SceneBegin(topR);
		if (vis_open)
			draw_vis_top(sk, par);
		else
			draw_top(sk, buffering, muted, par);
		C2D_TargetClear(bottom, sk->bottom_bg);
		C2D_SceneBegin(bottom);
		draw_bottom(sk);
		C3D_FrameEnd(0);
	}

quit:
	running = false;
	/* Everything starts stopping at once: the backend's threads, a list
	 * fetch mid-flight, these three. Quitting used to stop them one by
	 * one, each with seconds of grace. */
	native_quit_signal();
	native_fetch_cancel();
	/* The lister and art threads can sit in a TLS read for a while; the
	 * textures and buffers they use are freed below, so wait for them. */
	{
		const u64 t0 = osGetTime();
		threadJoin(lister, 8000000000ULL);
		const u64 t1 = osGetTime();
		threadJoin(snd, 2000000000ULL);
		threadJoin(art, 8000000000ULL);
		const u64 t2 = osGetTime();
		native_log("EXIT: lists %llu ms, covers %llu ms\n",
		           (unsigned long long)(t1 - t0),
		           (unsigned long long)(t2 - t1));
	}
	lid_mode_set(false);
	native_exit();
	C3D_TexDelete(&art_tex);
	thumbs_free_all();
	ui_exit();
	C2D_TextBufDelete(text_buf);
	C2D_Fini();
	C3D_Fini();
	ndspExit();
	mcuHwcExit();
	ptmuExit();
	socExit();
	aptUnhook(&apt_cookie);
	gfxExit();
	return 0;
}
