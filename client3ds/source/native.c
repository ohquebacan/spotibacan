#include "native.h"

#include "ap3ds.h"
#include "cast3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"
#include "track.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"

#include <3ds.h>
#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* The UI draws over the text console, so without a log this mode is opaque:
 * a failed fetch would show as an empty list and nothing else. Same approach
 * the probes use, and the same reason. */
static FILE *logf;
static u64 log_t0, log_flushed;
static volatile bool log_dirty;

/* Lines carry seconds since start, so a slow spot shows as a gap. They go
 * to a big buffer that the player thread flushes about once a second:
 * flushing every line made whichever thread logged, the UI too, wait on
 * an SD write, and a busy download queue logs a lot. */
static void write_log_line(const char *line)
{
	fputs(line, stderr);
	if (!logf) {
		/* Beside the .3dsx, like the token and the device id: everything the
		 * app owns lives in its own folder, so the card stays clean and the
		 * whole app is one directory to copy or delete. */
		char path[320];
		snprintf(path, sizeof(path), "%s", cfg_path());
		char *slash = strrchr(path, '/');
		if (slash)
			slash[1] = '\0';
		else
			path[0] = '\0';
		strncat(path, "native.log", sizeof(path) - strlen(path) - 1);
		logf = fopen(path, "a");
		if (logf)
			setvbuf(logf, NULL, _IOFBF, 16 * 1024);
		log_t0 = log_flushed = osGetTime();
	}
	if (logf) {
		const u64 ms = osGetTime() - log_t0;
		fprintf(logf, "[%5llu.%02llu] %s", ms / 1000, (ms % 1000) / 10,
		        line);
		log_dirty = true;
	}
}

/* Player thread: writes out what the last second logged. */
static void log_tick(bool now)
{
	if (!logf || !log_dirty)
		return;
	const u64 t = osGetTime();
	if (!now && t - log_flushed < 1000)
		return;
	log_dirty = false;
	log_flushed = t;
	fflush(logf);
}

static void nlog(const char *fmt, ...)
{
	char line[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	write_log_line(line);
}

/* The UI layer logs through here: the file lives in native.c and stays the
 * single place to read after a bug report. */
void native_log(const char *fmt, ...)
{
	char line[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	write_log_line(line);
}

static const char *view_name(native_view v)
{
	switch (v) {
	case NATIVE_PLAYLISTS: return "playlists";
	case NATIVE_LIKED:     return "liked";
	case NATIVE_SEARCH:    return "search";
	case NATIVE_RADIO:     return "radio";
	case NATIVE_TRACKS:    return "tracks";
	case NATIVE_DOWNLOADS: return "downloads";
	}
	return "?";
}

/* The whole file is reserved up front and filled in order by a download
 * thread while the decoder walks behind it (track_stream in track.h). How
 * far behind is a safety margin: stb_vorbis parses whole pages while
 * decoding, and a page can run to ~64 KB. */
#define STREAM_START_BYTES (32 * 1024)   /* enough for the Vorbis headers */
#define STREAM_MARGIN      (128 * 1024)
#define NATIVE_RATE   44100

/* A download job owns its track_stream on the heap, so a skipped song can
 * keep downloading (and saving itself) in the background while the player
 * is already on the next track: the skip no longer waits for the file. */
typedef struct dl_job {
	track_stream  ts;
	char          uri[64];   /* the requested track, for the queue to
	                          * re-ask for it */
	volatile bool stop;
	unsigned char tries;     /* the queue entry's failed tries so far */
	volatile bool save;      /* write the .ogg (+ cover) when complete */
	volatile bool saved;     /* CAS guard: exactly one save per job */
	/* Ownership handoff between the player and the download thread:
	 * 0 both alive, 1 player detached (thread frees), 2 thread finished
	 * (player joins and frees). The CAS makes the handoff race-free. */
	volatile int  handoff;
	bool          from_queue;   /* started by the download queue */
	int           core;         /* queue jobs: the core it was put on */
	volatile bool saving;       /* writing the file and its cover to SD */
	struct dl_job *next;
} dl_job;

/* The download queue: an armed download whose background slot is taken by a
 * newer one lands here instead of dying. Drained one at a time by the
 * player thread whenever no other download is using the link. */
typedef struct {
	char uri[64];
	char artist[128];
	char title[128];
	unsigned char tries;   /* failed starts so far; not kept on the card */
} dlq_entry;

/* The pump waits until this time after a failed start, so an outage costs
 * a retry every so often rather than the queue one entry per loop. */
static u64 dlq_hold_until;
/* When the last song stopped decoding: the idle loop leaves the queue
 * alone right after, while the UI is about to start the next one. */
static u64 play_ended_at;

static dlq_entry dlq[NATIVE_DLQ_MAX];
static int dlq_count;                    /* guarded by g.lock */
/* The queue's in-flight downloads (guarded by g.lock), two at once: one on
 * the system core, one at low priority on the app core. A single download
 * left the link idle through every save, cover fetch and next start, and
 * ran on half a shared core. Each job is detached the moment it starts, so
 * its thread clears its slot before it frees the job; anyone holding the
 * lock may read through them. */
#define DLQ_PAR 2
static struct dl_job *dlq_jobs[DLQ_PAR];
static char dlq_active_title[DLQ_PAR][128];
/* The entry each slot is working on. It stays in dlqueue.txt until its
 * file is saved: quitting mid-song used to drop it from the card, so the
 * next start never tried it again. */
static dlq_entry dlq_live[DLQ_PAR];
static bool dlq_live_on[DLQ_PAR];
/* A worker changed the queue; the player thread writes it out. */
static volatile bool dlq_save_pending;

static int dlq_active(void)   /* g.lock held */
{
	int n = 0;
	for (int i = 0; i < DLQ_PAR; i++)
		n += dlq_jobs[i] != NULL;
	return n;
}
/* Progress for the "12 / 87" readout, reset whenever the queue runs dry. */
static int dlq_done, dlq_failed;

/* Navigation away from a view whose fetch is still walking chunks sets
 * this; native_fetch clears it on entry and name_tracks honors it between
 * chunks. */
static volatile bool fetch_cancel;

static void (*progress_cb)(native_row *rows, int done, int total);

void native_set_progress_cb(void (*cb)(native_row *, int, int))
{
	progress_cb = cb;
}

void native_fetch_cancel(void) { fetch_cancel = true; }

static void save_download(const track_stream *ts);
static bool dlq_retry(const dlq_entry *e, bool spend, u64 hold_ms);
static void dlist_write(const char *title, const native_row *rows, int n);
static void download_path(const char *artist, const char *title,
                          char *out, size_t outlen);
static int rootlist_cache_load(native_row *rows, int max);

static struct {
	bool          ready;
	login5_token  l5;
	/* The login5 token lasts about an hour. It is renewed on the player
	 * thread from the stored credentials; readers on other threads take it
	 * through l5_token(), which flips between two buffers so a refresh
	 * never rewrites a token someone is in the middle of sending. */
	char          l5_tok[2][1024];
	volatile int  l5_cur;
	uint8_t       l5_cred[320];
	size_t        l5_cred_len;
	char          sp_host[128];
	char          username[128];
	ap_session   *ap;
	/* Keep-alive link to spclient for track metadata and storage-resolve.
	 * Player thread only; it saves a TLS handshake or two per track. */
	http_session *sp_sess;
	/* Everything a reconnect needs: the OAuth token the AP logs in with, the
	 * device id, and the candidate list apresolve gave at init. */
	char          access_token[1280];
	char          device_id[64];
	ap_candidate  cands[AP_MAX_CANDIDATES];
	int           cand_count;
	native_sink   sink;

	Thread        player;
	LightLock      lock;
	volatile bool  quit;
	struct dl_job *job;       /* download of the track now playing, or NULL */
	struct dl_job *jobs;      /* every live job, detached ones included */	/* Set by the UI thread, consumed by the player. file:<name>.ogg means a
	 * local download under the music folder, no network involved. */
	char           want[192];
	volatile bool  want_new;
	volatile bool  want_stop;
	/* Set when the console wakes from sleep: the wifi went down under
	 * every open link, which would otherwise each sit out a timeout. */
	volatile bool  woke;
	/* Seek request in samples, -1 = none. Set by the UI, consumed by the
	 * decoder, which may reject it when the target is not downloaded yet. */
	volatile long  want_seek_frame;

	volatile native_state state;
	char          title[128];
	char          artist[128];
	char          album[128];
	char          art[200];
	char          album_uri[40];
	char          artist_uri[40];
	char          profile_art[80];   /* the user's own avatar, for the idle hero */
	char          error[192];
	volatile unsigned frames_out;
	volatile unsigned total_frames;

	/* When on, every fully-downloaded track is also written to the SD as a
	 * plain .ogg beside the app (music/). The bytes are already here and
	 * already decrypted for playback; saving them is a courtesy copy. */
	volatile bool downloads_on;

	/* The download button on the player screen: arms a one-shot save of the
	 * track now playing. The file lands when its background download
	 * completes, which a skip interrupts -- so it means "let this one
	 * finish". Cleared when the track changes. */
	volatile bool want_save;
	/* Armed after the current file already landed: the player saves it. */
	volatile bool save_now;

	/* Offline mode: the player thread runs but never touches the network.
	 * Set once at init, read everywhere, never cleared. */
	bool          offline;
} g;

void native_set_downloads(bool on) { g.downloads_on = on; }

/* Network workers go to the system core when the app has been granted
 * time there (main sets that up): decrypting TLS and the audio file is
 * what the console's CPU spends most on, and on the app core it queued
 * behind drawing and decoding. Falls back to the app core. */
Thread native_spawn_worker(ThreadFunc fn, void *arg, size_t stack, int prio)
{
	Thread t = threadCreate(fn, arg, stack, prio, 1, false);
	if (!t)
		t = threadCreate(fn, arg, stack, prio, -2, false);
	return t;
}

/* One line of what the console has left: the heap (used, and free inside
 * it), the linear heap, and kernel threads. "DLQ: no thread for" ended
 * the queue after about eight songs even with every thread detached, and
 * this tells memory from the thread limit. */
static void mem_note(const char *why)
{
	struct mallinfo mi = mallinfo();
	const u32 heap = envGetHeapSize();
	s64 th_cur = -1, th_max = -1;
	Handle rl;
	ResourceLimitType t = RESLIMIT_THREAD;
	if (R_SUCCEEDED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
		svcGetResourceLimitCurrentValues(&th_cur, rl, &t, 1);
		svcGetResourceLimitLimitValues(&th_max, rl, &t, 1);
		svcCloseHandle(rl);
	}
	nlog("MEM %s: heap %u of %u KB used (%u KB free inside), "
	     "linear %u KB free, threads %lld/%lld\n", why,
	     (unsigned)mi.uordblks / 1024, (unsigned)heap / 1024,
	     (unsigned)(mi.fordblks + (heap > (u32)mi.arena ? heap - mi.arena : 0)) / 1024,
	     (unsigned)linearSpaceFree() / 1024, th_cur, th_max);
}

/* Largest block malloc still hands out, to tell a full heap from a
 * fragmented one. Only on failure paths: it walks the heap. */
static void mem_largest(void)
{
	size_t lo = 0, hi = envGetHeapSize();
	while (hi - lo > 64 * 1024) {
		size_t mid = lo + (hi - lo) / 2;
		void *p = malloc(mid);
		if (p) {
			free(p);
			lo = mid;
		} else {
			hi = mid;
		}
	}
	nlog("MEM largest free block about %u KB\n", (unsigned)(lo / 1024));
}

static const char *l5_token(void) { return g.l5_tok[g.l5_cur]; }

static void l5_publish(void)
{
	const int next = !g.l5_cur;
	snprintf(g.l5_tok[next], sizeof(g.l5_tok[next]), "%s", g.l5.access_token);
	if (g.l5.stored_len && g.l5.stored_len <= sizeof(g.l5_cred)) {
		memcpy(g.l5_cred, g.l5.stored, g.l5.stored_len);
		g.l5_cred_len = g.l5.stored_len;
	}
	__sync_synchronize();
	g.l5_cur = next;
}

/* Renews the library token ahead of its expiry (or right away when force is
 * set, after a 401). Without this, every metadata and browse call started
 * failing with HTTP 401 about an hour in. Any thread may call it: the
 * player only got to it when idle, and a song paused under a cast is never
 * idle, so casting and the lists went 401 an hour in. The lock keeps it to
 * one login5 at a time; whoever comes second finds the fresh token. */
static LightLock l5_lock;

static void l5_refresh(bool force)
{
	if (!g.l5_cred_len || g.offline)
		return;
	static time_t next_try;
	time_t now = time(NULL);
	if (!force && (g.l5.expires_at == 0 || now < g.l5.expires_at - 300))
		return;
	LightLock_Lock(&l5_lock);
	if (now < next_try) {
		LightLock_Unlock(&l5_lock);
		return;
	}
	next_try = now + 30;   /* a failing login5 is not retried in a tight loop */
	login5_token fresh;
	char err[192];
	if (login5_from_credentials(g.username, g.l5_cred, g.l5_cred_len,
	                            g.device_id, &fresh, err, sizeof(err)) != 0) {
		LightLock_Unlock(&l5_lock);
		nlog("L5: refresh failed: %s\n", err);
		return;
	}
	g.l5 = fresh;
	l5_publish();
	LightLock_Unlock(&l5_lock);
	nlog("L5: token refreshed\n");
}

void native_save_current(void)
{
	g.want_save = !g.want_save;
	LightLock_Lock(&g.lock);
	if (g.job) {
		g.job->save = g.want_save;
		/* The download may already be complete (a 3-minute song lands in
		 * seconds), so the download thread checked the flag long ago. The
		 * save itself (MBs to SD plus a cover fetch) used to run right
		 * here on the UI thread, freezing it, racing the job's teardown.
		 * The player thread does it instead. */
		if (g.want_save)
			g.save_now = true;
	}
	LightLock_Unlock(&g.lock);
}
bool native_save_armed(void) { return g.want_save; }

bool native_stream_busy(void)
{
	if (!g.ready)
		return false;
	LightLock_Lock(&g.lock);
	bool busy = g.job && !g.job->ts.dl_done && !g.job->ts.dl_failed;
	LightLock_Unlock(&g.lock);
	return busy;
}

int native_dl_queue_count(void)
{
	LightLock_Lock(&g.lock);
	int n = dlq_count;
	LightLock_Unlock(&g.lock);
	return n;
}

bool native_dl_queue_entry(int i, char *title, size_t titlelen)
{
	LightLock_Lock(&g.lock);
	bool ok = i >= 0 && i < dlq_count;
	if (ok)
		snprintf(title, titlelen, "%s", dlq[i].title);
	LightLock_Unlock(&g.lock);
	return ok;
}

int native_dl_active(char *title_out, size_t titlelen)
{
	/* With two running, the one further behind is the one worth showing. */
	int pct = -1;
	LightLock_Lock(&g.lock);
	for (int i = 0; i < DLQ_PAR; i++) {
		const struct dl_job *j = dlq_jobs[i];
		if (!j)
			continue;
		const track_stream *ts = &j->ts;
		int p = ts->len ? (int)((uint64_t)ts->downloaded * 100 / ts->len) : 0;
		if (p > 100)
			p = 100;
		if (j->saving)
			p = NATIVE_DL_SAVING;
		if (pct < 0 || p < pct) {
			pct = p;
			snprintf(title_out, titlelen, "%s", dlq_active_title[i]);
		}
	}
	LightLock_Unlock(&g.lock);
	return pct;
}

void native_dl_progress(int *done, int *total)
{
	LightLock_Lock(&g.lock);
	*done = dlq_done + dlq_failed;
	*total = *done + dlq_count + dlq_active();
	LightLock_Unlock(&g.lock);
}

/* The queue survives a restart: one "uri<TAB>artist<TAB>title" per line
 * beside the config. Rewritten whole on every change, which is a few KB at
 * most and happens once per track. */
static void dlq_file(char *out, size_t n)
{
	char d[320];
	cfg_dir(d, sizeof(d));
	snprintf(out, n, "%sdlqueue.txt", d);
}

static void dlq_save(void)
{
	char path[352], tmp[360];
	dlq_file(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	LightLock_Lock(&g.lock);
	for (int i = 0; i < DLQ_PAR; i++)
		if (dlq_live_on[i])
			fprintf(f, "%s\t%s\t%s\n", dlq_live[i].uri,
			        dlq_live[i].artist, dlq_live[i].title);
	for (int i = 0; i < dlq_count; i++)
		fprintf(f, "%s\t%s\t%s\n", dlq[i].uri, dlq[i].artist, dlq[i].title);
	LightLock_Unlock(&g.lock);
	if (fclose(f) == 0) {
		remove(path);
		rename(tmp, path);
	} else {
		remove(tmp);
	}
}

/* Appends a track to the download queue, unless it is already queued.
 * Caller holds no lock. Returns false when the queue is full. */
static bool dlq_add(const char *uri, const char *artist, const char *title,
                    bool quiet)
{
	LightLock_Lock(&g.lock);
	for (int i = 0; i < dlq_count; i++)
		if (strcmp(dlq[i].uri, uri) == 0) {
			LightLock_Unlock(&g.lock);
			return true;   /* already waiting: same outcome */
		}
	if (dlq_count >= NATIVE_DLQ_MAX) {
		LightLock_Unlock(&g.lock);
		nlog("DLQ: cola llena, se descarta %s\n", title);
		return false;
	}
	dlq_entry *e = &dlq[dlq_count++];
	snprintf(e->uri, sizeof(e->uri), "%s", uri);
	snprintf(e->artist, sizeof(e->artist), "%s", artist);
	snprintf(e->title, sizeof(e->title), "%s", title);
	LightLock_Unlock(&g.lock);
	if (!quiet)
		nlog("DLQ: en cola %s - %s\n", artist, title);
	return true;
}

static bool dlq_enqueue(const char *uri, const char *artist,
                        const char *title)
{
	bool ok = dlq_add(uri, artist, title, false);
	dlq_save();
	return ok;
}

int native_dl_enqueue_list(const native_row *rows, int n, const char *title)
{
	if (!g.ready || g.offline)
		return 0;
	dlist_write(title, rows, n);   /* so Downloads can show it as a folder */
	/* Downloading a list again (songs were added to it since) queues only
	 * what the card lacks: the rest would be skipped one by one anyway,
	 * but would still crowd the queue and its "12 / 87" count. */
	int added = 0, have = 0, before = native_dl_queue_count();
	for (int i = 0; i < n; i++) {
		const native_row *r = &rows[i];
		if (r->is_playlist || strncmp(r->uri, "spotify:track:", 14) != 0)
			continue;
		char path[520];
		struct stat st;
		download_path(r->sub, r->label, path, sizeof(path));
		if (stat(path, &st) == 0 && st.st_size > 0) {
			have++;
			continue;
		}
		if (!dlq_add(r->uri, r->sub, r->label, true))
			break;   /* full */
	}
	added = native_dl_queue_count() - before;
	nlog("DLQ: %d of %d tracks queued from a list (%d already on the card)\n",
	     added, n, have);
	dlq_save();
	return added;
}

void native_dl_clear(void)
{
	LightLock_Lock(&g.lock);
	dlq_count = 0;
	LightLock_Unlock(&g.lock);
	dlq_save();
}

static void dlq_load(void)
{
	char path[352];
	dlq_file(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[400];
	while (fgets(line, sizeof(line), f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = '\0';
		char *a = strchr(line, '\t');
		char *t = a ? strchr(a + 1, '\t') : NULL;
		if (!t)
			continue;
		*a++ = '\0';
		*t++ = '\0';
		if (strncmp(line, "spotify:track:", 14) == 0)
			dlq_add(line, a, t, true);
	}
	fclose(f);
	nlog("DLQ: %d tracks restored from the card\n", native_dl_queue_count());
}

bool native_ready(void) { return g.ready; }
bool native_offline(void) { return g.offline; }
const char *native_account(void) { return g.username; }

static void step(void (*progress)(const char *), const char *what)
{
	nlog("INIT: %s\n", what);
	if (progress)
		progress(what);
}

bool native_has_token(void)
{
	char stored[512];
	return cfg_load_refresh_token(stored, sizeof(stored));
}

int native_pair(void (*show_code)(const char *code, const char *url),
                bool (*poll)(int attempt), char *err, size_t errlen)
{
	/* Pairing needs TLS but not a session, and it runs before native_init,
	 * so it brings up what it needs itself. Both calls are no-ops the second
	 * time around. */
	if (R_FAILED(psInit())) {
		snprintf(err, errlen, "no entropy source (ps:ps)");
		return -1;
	}
	int rc = tls_global_init();
	if (rc) {
		tls_strerror(rc, err, errlen);
		return -1;
	}

	pair_request req;
	if (pair_begin(&req, err, errlen) != 0)
		return -1;
	nlog("PAIR: code %s\n", req.user_code);
	/* Always the bare verification_uri: the _complete variant embeds the code
	 * in the URL, and on screen it read as if the user had to type the whole
	 * long address instead of just the short code below it. */
	if (show_code)
		show_code(req.user_code, req.verification_uri);

	int interval = req.interval_s > 0 ? req.interval_s : 5;
	time_t deadline = time(NULL) + req.expires_in_s;
	for (int attempt = 1; time(NULL) < deadline; attempt++) {
		/* The caller owns the frame loop, so it does the waiting and can
		 * keep the code on screen and the buttons alive. */
		for (int i = 0; i < interval * 60; i++)
			if (poll && !poll(attempt)) {
				snprintf(err, errlen, "cancelled");
				return -1;
			}

		spotify_tokens tok;
		char perr[192] = "";
		pair_status st = pair_poll(&req, &tok, perr, sizeof(perr));
		if (st == PAIR_OK) {
			if (!tok.refresh_token[0] ||
			    !cfg_save_refresh_token(tok.refresh_token, tok.scope)) {
				snprintf(err, errlen, "could not save the token to the SD card");
				return -1;
			}
			nlog("PAIR: ok\n");
			return 0;
		}
		if (st == PAIR_SLOW_DOWN) {
			interval += 5;
			continue;
		}
		if (st != PAIR_PENDING) {
			snprintf(err, errlen, "%s", perr[0] ? perr : "pairing failed");
			return -1;
		}
	}
	snprintf(err, errlen, "the code expired, start the app again");
	return -1;
}

native_state native_playback_state(void) { return g.state; }
const char *native_now_title(void)  { return g.title; }
const char *native_now_artist(void) { return g.artist; }
const char *native_now_album(void)  { return g.album; }
const char *native_now_album_uri(void)  { return g.album_uri; }
const char *native_now_artist_uri(void) { return g.artist_uri; }
/* The URI last asked for, so the lyrics view can tell its document belongs
 * to what is playing. Empty before the first play. */
const char *native_now_uri(void)    { return g.want; }
const char *native_now_art(void)    { return g.art; }
const char *native_profile_art(void) { return g.profile_art; }

/* One retry of the avatar fetch, from the art worker. No-op once the URL is
 * known, so the caller can poll on a timer without tracking state itself.
 * Returns the server's Retry-After seconds on a 429, 0 on success, -1 on any
 * other failure. */
int native_profile_art_refresh(void)
{
	if (!g.ready || g.profile_art[0] || !g.access_token[0])
		return -1;
	char url[80];
	char perr[128] = "";
	int retry_after = 0;
	if (sp_user_image(g.access_token, url, sizeof(url), perr,
	                  sizeof(perr), &retry_after) == 0) {
		snprintf(g.profile_art, sizeof(g.profile_art), "%s", url);
		/* Cache it: /v1/me is so rate-limited that a success may not repeat
		 * itself for hours, and the CDN URL outlives the session anyway. */
		cfg_save_profile_art(url);
		nlog("INIT: foto de perfil recuperada\n");
		return 0;
	}
	nlog("INIT: foto de perfil sigue sin llegar: %s\n", perr);
	return retry_after > 0 ? retry_after : -1;
}
const char *native_last_error(void) { return g.error; }

/* Likes/unlikes any track URI with whatever access token is current. Runs
 * from the art worker; mirrors sp_library_modify's contract, including the
 * 403-means-re-pair return. */
int native_like(const char *uri, bool like, char *err, size_t errlen,
                int *retry_after)
{
	const char *id = strrchr(uri, ':');
	if (!id || !id[1] || !g.access_token[0]) {
		snprintf(err, errlen, "invalid uri or token");
		return -1;
	}
	return sp_library_modify(g.access_token, id + 1, like, err, errlen,
	                         retry_after);
}

/* Builds the music/ path a track saves to, so the writer and the "already
 * saved?" check can never disagree on the name. */
/* The file name a song is saved under: "<artist> - <title>.ogg", minus
 * the characters FAT refuses. */
/* Copies src into name from n on, dropping characters FAT refuses, up to
 * cap bytes; a cut never splits a UTF-8 sequence (a stray lead byte made
 * an unopenable name). Returns the new length. */
static int name_part(char *name, int n, const char *src, int cap)
{
	for (const char *p = src; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '"' ||
		    *p == '?' || *p == '*' || *p == '<' || *p == '>' || *p == '|')
			continue;
		int len = 1;
		const unsigned char c = (unsigned char)*p;
		if (c >= 0xF0) len = 4;
		else if (c >= 0xE0) len = 3;
		else if (c >= 0xC0) len = 2;
		if (n + len > cap)
			break;
		for (int k = 0; k < len && p[k]; k++)
			name[n++] = p[k];
		p += len - 1;
	}
	return n;
}

static void download_name(const char *artist, const char *title,
                          char *name, size_t namelen)
{
	int n = name_part(name, 0, artist, 60);
	n += snprintf(name + n, namelen - n, " - ");
	n = name_part(name, n, title, 130);
	snprintf(name + n, namelen - n, ".ogg");
}

static void download_path(const char *artist, const char *title,
                          char *out, size_t outlen)
{
	char dir[352];
	cfg_dir(dir, sizeof(dir));
	strncat(dir, "music", sizeof(dir) - strlen(dir) - 1);
	mkdir(dir, 0777);
	char name[160];
	download_name(artist, title, name, sizeof(name));
	snprintf(out, outlen, "%s/%s", dir, name);
}

/* A saved song is only "Artist - Title.ogg"; casting it needs its Spotify
 * id back. Every save notes the pair in music/ids.txt, one tab-separated
 * line per file. Workers save side by side, hence the lock. */
static LightLock ids_lock;

static void file_id_path(char *out, size_t n)
{
	cfg_dir(out, n);
	strncat(out, "music/ids.txt", n - strlen(out) - 1);
}

static void file_id_note(const char *name, const char *uri)
{
	if (!name[0] || strncmp(uri, "spotify:track:", 14) != 0)
		return;
	char path[360];
	file_id_path(path, sizeof(path));
	LightLock_Lock(&ids_lock);
	FILE *f = fopen(path, "a");
	if (f) {
		fprintf(f, "%s\t%s\n", name, uri);
		fclose(f);
	}
	LightLock_Unlock(&ids_lock);
}

static bool file_id_find(const char *name, char *uri, size_t urilen)
{
	char path[360];
	file_id_path(path, sizeof(path));
	bool found = false;
	LightLock_Lock(&ids_lock);
	FILE *f = fopen(path, "r");
	if (f) {
		char line[320];
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			char *tab = strchr(line, '\t');
			if (!tab)
				continue;
			*tab = '\0';
			if (strcmp(line, name) == 0) {
				snprintf(uri, urilen, "%s", tab + 1);
				found = true;   /* read on: a later line is a newer save */
			}
		}
		fclose(f);
	}
	LightLock_Unlock(&ids_lock);
	return found;
}

/* Whether the playing track already sits on the card. One stat per track:
 * the answer is cached against the URI, because the UI asks every frame. */
static char saved_checked_uri[192];
static char saved_checked_title[128];
static bool saved_flag;

bool native_current_saved(void)
{
	if (strncmp(g.want, "file:", 5) == 0)
		return true;   /* a local file is on the card by definition */
	if (!g.title[0])
		return false;
	/* Keyed on the title too: while a new song loads, g.want is already
	 * the new uri but g.title still names the old one. */
	if (strcmp(saved_checked_uri, g.want) != 0 ||
	    strcmp(saved_checked_title, g.title) != 0) {
		snprintf(saved_checked_uri, sizeof(saved_checked_uri), "%s", g.want);
		snprintf(saved_checked_title, sizeof(saved_checked_title), "%s",
		         g.title);
		char path[520];
		download_path(g.artist, g.title, path, sizeof(path));
		struct stat st;
		saved_flag = stat(path, &st) == 0 && st.st_size > 0;
	}
	return saved_flag;
}

/* Writes the finished download as a plain .ogg under music/ beside the app.
 * Only called once dl_done: a partial file is worthless. The Spotify key
 * header is stripped, so what lands is the Ogg stream itself. The write is
 * staged under a .tmp name and renamed, so a kill mid-write never leaves a
 * truncated file that the "already saved?" check would trust. */
static void save_download(const track_stream *ts)
{
	char path[520];
	download_path(ts->meta.artist, ts->meta.name, path, sizeof(path));

	/* A length under the key header would underflow into a dump of the
	 * heap; anything that is not an Ogg stream is not worth keeping. */
	if (!ts->data || ts->len <= SPOTIFY_OGG_HEADER + 4 ||
	    memcmp(ts->data + SPOTIFY_OGG_HEADER, "OggS", 4) != 0) {
		nlog("DL: not saving %s (no Ogg stream)\n", path);
		return;
	}

	char tmp[528];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		nlog("DL: could not open %s\n", tmp);
		return;
	}
	/* A full card fails the write or the close; renaming the stub anyway
	 * would leave a truncated .ogg that every "already saved?" check
	 * trusts from then on. */
	const size_t want = ts->len - SPOTIFY_OGG_HEADER;
	bool ok = fwrite(ts->data + SPOTIFY_OGG_HEADER, 1, want, f) == want;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		nlog("DL: write failed for %s (SD card full?)\n", path);
		return;
	}
	/* The saved check caches per URI; make it look again. */
	saved_checked_uri[0] = '\0';
	nlog("DL: guardada %s (%.1f MB)\n", path,
	     (double)(ts->len - SPOTIFY_OGG_HEADER) / (1024.0 * 1024.0));
	{
		char name[160];
		download_name(ts->meta.artist, ts->meta.name, name, sizeof(name));
		file_id_note(name, ts->meta.uri);
	}

	/* The cover goes beside the audio as <same name>.jpg, so the offline
	 * shelf can show it without any network. A failure here only costs the
	 * picture, never the download itself. */
	if (ts->meta.art_large[0]) {
		char jpg[520];
		snprintf(jpg, sizeof(jpg), "%s", path);
		char *dot = strrchr(jpg, '.');
		if (dot)
			strcpy(dot, ".jpg");
		struct stat st;
		if (stat(jpg, &st) != 0 || st.st_size == 0) {
			http_reply r;
			memset(&r, 0, sizeof(r));
			if (http_get_url(ts->meta.art_large, 1024 * 1024, &r) == 0 &&
			    (r.status == 200 || r.status == 206) && r.len > 0) {
				FILE *jf = fopen(jpg, "wb");
				if (jf) {
					fwrite(r.body, 1, r.len, jf);
					fclose(jf);
					nlog("DL: caratula guardada %s\n", jpg);
				}
			} else {
				nlog("DL: no se pudo bajar la caratula (%s)\n",
				     r.err[0] ? r.err : "http");
			}
			http_reply_free(&r);
		}
	}
}

float native_position(void)
{
	return (float)g.frames_out / NATIVE_RATE;
}

void native_seek(float seconds)
{
	if (seconds < 0.0f)
		seconds = 0.0f;
	long frame = (long)(seconds * NATIVE_RATE);
	__sync_synchronize();
	g.want_seek_frame = frame;
}

float native_duration(void)
{
	return (float)g.total_frames / NATIVE_RATE;
}

void native_play(const char *track_uri)
{
	if (!g.ready || !track_uri)
		return;
	nlog("UI: play pedido %s\n", track_uri);
	LightLock_Lock(&g.lock);
	snprintf(g.want, sizeof(g.want), "%s", track_uri);
	g.want_new = true;
	g.want_stop = false;
	LightLock_Unlock(&g.lock);
}

void native_stop(void)
{
	g.want_stop = true;
}

/* Fills the job's buffer and saves the file if it was armed. The handoff
 * decides who frees: a job the player already detached belongs to this
 * thread; otherwise the player joins and reclaims it. */
static void download_thread(void *arg)
{
	dl_job *job = arg;
	const u64 t0 = osGetTime();
	track_stream_download(&job->ts, &job->stop);
	/* Throughput tells a slow CDN from a slow console when a song stalls. */
	{
		const u64 ms = osGetTime() - t0;
		const unsigned kb = (unsigned)(job->ts.downloaded / 1024);
		nlog("DL: %s %u KB in %llu ms (%u KB/s)%s%s%s\n",
		     job->ts.dl_done ? "done" : (job->stop ? "stopped" : "FAILED"),
		     kb, (unsigned long long)ms, ms ? (unsigned)(kb * 1000ull / ms) : 0,
		     job->from_queue ? (job->core ? " [queue, sys core]"
		                                  : " [queue, app core]") : "",
		     job->ts.dl_err[0] ? ": " : "", job->ts.dl_err);
	}
	/* Saving on the way out would hold HOME hostage behind an SD write
	 * plus a cover fetch: quitting drops the save, the queue entry was
	 * already consumed anyway. */
	if (job->save && !g.quit && job->ts.dl_done && !job->ts.dl_failed &&
	    !__sync_lock_test_and_set(&job->saved, 1)) {
		job->saving = true;   /* a 320 kbps file is seconds of SD writes */
		save_download(&job->ts);
		job->saving = false;
	}
	if (job->from_queue) {
		const bool ok = job->ts.dl_done && !job->ts.dl_failed;
		dlq_entry e;
		bool have = false;
		LightLock_Lock(&g.lock);
		for (int i = 0; i < DLQ_PAR; i++)
			if (dlq_jobs[i] == job) {
				dlq_jobs[i] = NULL;
				/* Quitting keeps it on the card for the next start. */
				if (!g.quit) {
					e = dlq_live[i];
					have = dlq_live_on[i];
					dlq_live_on[i] = false;
				}
			}
		if (ok)
			dlq_done++;
		LightLock_Unlock(&g.lock);
		/* A song that broke off (a lost link, a CDN error) goes to the
		 * back of the line for another try, like a start that failed;
		 * it used to just count as failed. */
		if (!ok && !g.quit) {
			e.tries = job->tries;
			if (!have || !dlq_retry(&e, true, 20000)) {
				LightLock_Lock(&g.lock);
				dlq_failed++;
				LightLock_Unlock(&g.lock);
			} else {
				nlog("DLQ: %s broke off, back in the queue\n", e.title);
			}
		}
		if (!g.quit)
			dlq_save_pending = true;
	}
	__sync_synchronize();
	if (__sync_bool_compare_and_swap(&job->handoff, 0, 2))
		return;   /* the player is still around: it joins and frees */
	LightLock_Lock(&g.lock);
	dl_job **p = &g.jobs;
	while (*p && *p != job)
		p = &(*p)->next;
	if (*p)
		*p = job->next;
	LightLock_Unlock(&g.lock);
	const bool queued = job->from_queue;
	track_stream_free(&job->ts);
	free(job);
	if (queued)
		mem_note("queue done");
}

/* Download workers: a few long-lived threads that take one job at a time.
 * A thread per song leaked one of the 32 kernel threads this app may hold
 * on every download, detached or not (the log counted them up song by
 * song), until "DLQ: no thread for" ended the queue and any further play.
 * Workers 0-3 sit on the system core like every network worker; the last
 * one is on the app core, for the queue's second slot. */
#define DL_WORKERS 5
#define DL_APP_WORKER (DL_WORKERS - 1)
static struct dl_worker {
	Thread        th;
	LightEvent    go;
	dl_job       *volatile job;
	volatile bool taken;   /* reserved, or running a job */
} dlw[DL_WORKERS];

static void dl_worker_main(void *arg)
{
	struct dl_worker *w = arg;
	while (!g.quit) {
		LightEvent_WaitTimeout(&w->go, 500000000LL);   // 0.5 s
		dl_job *job = w->job;
		if (!job)
			continue;
		download_thread(job);   /* may free the job: not touched after */
		w->job = NULL;
		__sync_synchronize();
		w->taken = false;
	}
}

/* Reserves an idle worker on the system core (app_core false) or the app
 * core, starting its thread the first time. -1 when all are busy. */
static int dl_reserve(bool app_core)
{
	int pick = -1;
	LightLock_Lock(&g.lock);
	for (int i = 0; i < DL_WORKERS && pick < 0; i++)
		if (!dlw[i].taken && (i == DL_APP_WORKER) == app_core)
			pick = i;
	if (pick >= 0)
		dlw[pick].taken = true;
	LightLock_Unlock(&g.lock);
	if (pick < 0)
		return -1;
	struct dl_worker *w = &dlw[pick];
	if (!w->th) {
		LightEvent_Init(&w->go, RESET_ONESHOT);
		/* The fetch runs a full TLS handshake (cert chain + RSA bignum)
		 * on this stack; 48 KB was not always enough. */
		w->th = app_core
		    ? threadCreate(dl_worker_main, w, 96 * 1024, 0x38, 0, false)
		    : native_spawn_worker(dl_worker_main, w, 96 * 1024, 0x31);
		if (!w->th) {
			w->taken = false;
			mem_note("no worker");
			return -1;
		}
	}
	return pick;
}

static void dl_release(int i)
{
	dlw[i].taken = false;
}

static void dl_run(int i, dl_job *job)
{
	dlw[i].job = job;
	__sync_synchronize();
	LightEvent_Signal(&dlw[i].go);
}

/* For the player: a song that was just skipped can hold a worker for the
 * moment its stop takes to land, so a new one waits a little for it. */
static int dl_reserve_wait(void)
{
	for (int i = 0; i < 40; i++) {
		const int w = dl_reserve(false);
		if (w >= 0 || g.quit)
			return w;
		svcSleepThread(50000000LL);   // 50 ms
	}
	return -1;
}

/* Leaves a job with its download thread (the player is moving on). When the
 * thread already finished, the job is reclaimed here instead. */
static void job_detach(dl_job *job)
{
	__sync_synchronize();
	if (__sync_bool_compare_and_swap(&job->handoff, 0, 1))
		return;   /* the worker frees the job when it finishes */
	/* Finished already: the worker is done with it, nothing to wait for. */
	LightLock_Lock(&g.lock);
	dl_job **p = &g.jobs;
	while (*p && *p != job)
		p = &(*p)->next;
	if (*p)
		*p = job->next;
	LightLock_Unlock(&g.lock);
	track_stream_free(&job->ts);
	free(job);
}

/* Puts a failed entry back at the end of the queue for another go,
 * up to a few tries; false when it is spent. `spend` is false when the
 * song itself was never tried (no thread), so it keeps its tries. */
static bool dlq_retry(const dlq_entry *e, bool spend, u64 hold_ms)
{
	if (spend && e->tries >= 2)
		return false;
	bool ok = false;
	LightLock_Lock(&g.lock);
	if (dlq_count < NATIVE_DLQ_MAX) {
		dlq[dlq_count] = *e;
		if (spend)
			dlq[dlq_count].tries++;
		dlq_count++;
		ok = true;
	}
	LightLock_Unlock(&g.lock);
	dlq_hold_until = osGetTime() + hold_ms;
	return ok;
}

/* Key refusals in a row. One is a song Spotify will not give out; two
 * running is Spotify rationing keys after too many asks, and asking again
 * at once only extends it, so the queue sits out a long pause. */
static int dlq_refusals;

/* Starts the next queued download when nothing else is downloading. Only
 * ever called on the player thread, the owner of g.ap: the audio key rides
 * the access point session. Returns at once when there is nothing to do;
 * starting costs a few seconds (metadata, storage-resolve, key), so the
 * player only calls it with a well-filled ring. The download runs detached
 * on its own thread and saves itself when it lands. */
static void dlq_pump(void)
{
	if (!g.ready || g.offline || !g.ap || g.quit)
		return;
	if (osGetTime() < dlq_hold_until)
		return;
	dlq_entry e;
	bool take = false;
	LightLock_Lock(&g.lock);
	int slot = -1;
	for (int i = 0; i < DLQ_PAR && slot < 0; i++)
		if (!dlq_jobs[i])
			slot = i;
	/* The second slot only for 160 kbps files: two 320 ones next to a
	 * stream could pass what this console's RAM holds. */
	if (slot > 0 && sp_high_quality())
		slot = -1;
	if (slot >= 0 && dlq_count > 0) {
		take = true;
		for (dl_job *p = g.jobs; p; p = p->next)
			if (!p->from_queue && !p->ts.dl_done && !p->ts.dl_failed)
				take = false;   /* the song playing is still loading */
	}
	if (take) {
		e = dlq[0];
		memmove(&dlq[0], &dlq[1], (size_t)(dlq_count - 1) * sizeof(dlq[0]));
		dlq_count--;
		dlq_live[slot] = e;
		dlq_live_on[slot] = true;
	} else if (dlq_active() == 0 && dlq_count == 0) {
		dlq_done = dlq_failed = 0;   /* ran dry: the next batch counts anew */
	}
	LightLock_Unlock(&g.lock);
	if (!take)
		return;
	dlq_save();

	/* Saved another way while it waited? Nothing to do. */
	char path[520];
	download_path(e.artist, e.title, path, sizeof(path));
	struct stat st;
	if (stat(path, &st) == 0 && st.st_size > 0) {
		LightLock_Lock(&g.lock);
		dlq_done++;
		dlq_live_on[slot] = false;
		LightLock_Unlock(&g.lock);
		dlq_save();
		return;
	}

	dl_job *job = calloc(1, sizeof(*job));
	if (!job) {
		nlog("DLQ: no memory for %s\n", e.title);
		mem_note("no job");
		dlq_live_on[slot] = false;
		dlq_retry(&e, false, 60000);
		dlq_save();
		return;
	}
	/* A worker is reserved before the key is asked for: starting a song
	 * fetches its key, and a start that then found no thread threw that
	 * key away. Forty of those in a row got the account's keys refused
	 * for every song after. */
	job->core = slot == 0 ? 1 : 0;
	const int worker = dl_reserve(slot != 0);
	if (worker < 0) {
		nlog("DLQ: no thread for %s\n", e.title);
		mem_note("no thread");
		mem_largest();
		free(job);
		dlq_live_on[slot] = false;
		dlq_retry(&e, false, 60000);
		dlq_save();
		return;
	}

	char err[192] = "";
	snprintf(job->uri, sizeof(job->uri), "%s", e.uri);
	/* Only the card copy gets the bigger file: streaming 320 kbps
	 * outran this console's link and songs took half a minute. */
	job->ts.want_hq = sp_high_quality();
	job->ts.sess = g.sp_sess;
	int rc = track_stream_begin(g.sp_host, l5_token(), g.ap, e.uri, NULL,
	                            &job->ts, err, sizeof(err));
	if (rc != 0 && strstr(err, "401")) {
		l5_refresh(true);
		job->ts.want_hq = sp_high_quality();
		job->ts.sess = g.sp_sess;
		rc = track_stream_begin(g.sp_host, l5_token(), g.ap, e.uri,
		                        NULL, &job->ts, err, sizeof(err));
	}
	if (rc != 0) {
		nlog("DLQ: could not start %s (%s)\n", e.title, err);
		dl_release(worker);
		dlq_live_on[slot] = false;
		track_stream_free(&job->ts);
		free(job);
		const bool refused = strstr(err, "0x0002") != NULL;
		dlq_refusals = refused ? dlq_refusals + 1 : 0;
		u64 hold = 20000;
		if (dlq_refusals >= 2) {
			hold = 10 * 60000;
			nlog("DLQ: keys refused %d times running, pausing 10 min\n",
			     dlq_refusals);
		}
		/* A dropped link or a 5xx passes: back of the line, after a pause.
		 * Rationed keys are not the song's fault: it keeps its tries. */
		if (!dlq_retry(&e, dlq_refusals < 2, hold)) {
			LightLock_Lock(&g.lock);
			dlq_failed++;
			LightLock_Unlock(&g.lock);
		}
		dlq_save();
		return;
	}
	dlq_refusals = 0;
	job->save = true;
	job->from_queue = true;
	job->tries = e.tries;
	LightLock_Lock(&g.lock);
	job->next = g.jobs;
	g.jobs = job;
	dlq_jobs[slot] = job;
	snprintf(dlq_active_title[slot], sizeof(dlq_active_title[slot]),
	         "%s - %s", e.artist, e.title);
	LightLock_Unlock(&g.lock);
	nlog("DLQ: downloading %s (slot %d)\n", e.title, slot);
	mem_note("queue start");
	job_detach(job);   /* the worker frees the job when it is done */
	dl_run(worker, job);
}

/* Decodes one streaming track into the ring until it ends, the download
 * fails, the UI asks for something else, or the session is shutting down. */
static void play_stream(track_stream *ts)
{
	/* Wait for enough of the file to hold the Vorbis headers. */
	for (;;) {
		if (g.quit || g.want_stop || g.want_new)
			return;
		if (ts->dl_failed) {
			snprintf(g.error, sizeof(g.error), "%s",
			         ts->dl_err[0] ? ts->dl_err : "the download failed");
			g.state = NATIVE_FAILED;
			return;
		}
		__sync_synchronize();
		if (ts->data && ts->downloaded >= STREAM_START_BYTES)
			break;
		svcSleepThread(20000000LL);   // 20 ms
	}

	if (memcmp(ts->data + SPOTIFY_OGG_HEADER, "OggS", 4) != 0) {
		snprintf(g.error, sizeof(g.error),
		         "no Ogg stream after the %d byte header -- wrong key?",
		         SPOTIFY_OGG_HEADER);
		g.state = NATIVE_FAILED;
		return;
	}

	int verr = 0;
	stb_vorbis *v = stb_vorbis_open_memory(ts->data + SPOTIFY_OGG_HEADER,
	                                       (int)(ts->len - SPOTIFY_OGG_HEADER),
	                                       &verr, NULL);
	if (!v) {
		snprintf(g.error, sizeof(g.error),
		         "the decoder rejected the stream (%d)", verr);
		g.state = NATIVE_FAILED;
		return;
	}

	/* The length comes from the metadata: asking stb_vorbis would scan the
	 * tail of the file, which is still zeroed while the download runs. */
	g.total_frames =
	    (unsigned)((uint64_t)ts->meta.duration_ms * NATIVE_RATE / 1000);
	stb_vorbis_set_length(v, g.total_frames);
	g.frames_out = 0;
	g.state = NATIVE_PLAYING;
	nlog("PLAY %s - %s\n", g.artist, g.title);

	static s16 pcm[4096 * 2];
	long pending = -1;            /* far seek waiting on the download */
	unsigned pending_prev = 0;    /* where the stream was when it started */
	unsigned long long ka = 0;    /* last AP keepalive check */
	while (!g.quit && !g.want_stop && !g.want_new) {
		log_tick(false);
		/* Nobody else answers the access point while a song plays, and a
		 * session that misses pings for minutes is dead by the next track:
		 * that was the stuck "cargando" after a download, only visible when
		 * the user moved on. A cheap poll every second keeps it alive and
		 * marks it dead early, so the next fetch reconnects instead of
		 * timing out. */
		unsigned long long now = osGetTime();
		if (g.ap && now - ka >= 1000) {
			ka = now;
			if (ap_wait(g.ap, 0) == 1) {
				static uint8_t ka_buf[2048];
				uint8_t cmd;
				int kn = ap_recv(g.ap, &cmd, ka_buf, sizeof(ka_buf));
				if (kn < 0) {
					nlog("AP: sesion perdida durante la reproduccion\n");
					ap_close(g.ap);
					g.ap = NULL;
				} else if (cmd == AP_PKT_PING) {
					ap_handle_ping(g.ap, ka_buf, (size_t)kn);
				}
			}
		}
		if (g.save_now) {
			g.save_now = false;
			dl_job *cur = g.job;   /* only this thread changes g.job */
			if (cur && &cur->ts == ts && ts->dl_done && !ts->dl_failed &&
			    cur->save && !__sync_lock_test_and_set(&cur->saved, 1))
				save_download(ts);
		}
		/* With this track's file complete and seconds of audio queued in
		 * the ring, the link is idle: let the download queue use it. The
		 * start blocks a few seconds, which the full ring covers. */
		if (ts->dl_done && dlq_count > 0 && dlq_active() < DLQ_PAR &&
		    g.sink.free_space() < NATIVE_PUMP_FREE_MAX &&
		    g.total_frames > g.frames_out + NATIVE_PUMP_MIN_LEFT_S * NATIVE_RATE)
			dlq_pump();
		long want = g.want_seek_frame;
		if (want >= 0) {
			g.want_seek_frame = -1;
			__sync_synchronize();
			if ((unsigned)want >= g.total_frames)
				want = (long)g.total_frames - 1;
			unsigned prev = pending >= 0 ? pending_prev : g.frames_out;
			if (stb_vorbis_seek(v, (unsigned)want)) {
				/* Inside the downloaded span the jump is immediate. */
				g.frames_out = (unsigned)want;
				pending = -1;
				nlog("SEEK ok -> %ld\n", want);
			} else if (ts->len) {
				stb_vorbis_seek(v, prev);
				/* Past the watermark the buffer is zero fill and no page
				 * scan can land there: move the download to the target
				 * and wait the moment it takes the bytes to arrive. */
				size_t at = (size_t)((double)want /
				                     (double)g.total_frames *
				                     (double)ts->len) & ~(size_t)15;
				ts->dl_restart_at = (long)at;
				__sync_synchronize();
				pending = want;
				pending_prev = prev;
				g.frames_out = (unsigned)want;   /* the bar shows the goal */
				nlog("SEEK lejos -> %ld (byte %zu)\n", want, at);
			} else {
				stb_vorbis_seek(v, prev);
			}
		}
		if (pending >= 0) {
			if (ts->dl_failed) {
				pending = -1;
				g.frames_out = pending_prev;
			} else if (stb_vorbis_seek(v, (unsigned)pending)) {
				nlog("SEEK lejos aterrizo -> %ld\n", pending);
				pending = -1;
			} else {
				/* Nothing honest to play until the target page exists:
				 * keep the stream parked and let the ring run dry, which
				 * the UI already shows as buffering. */
				stb_vorbis_seek(v, pending_prev);
				svcSleepThread(10000000LL);   // 10 ms
				continue;
			}
		}
		/* Only decode when the ring has room: the ring is the pacing, and
		 * running ahead of it would just burn battery. */
		if (g.sink.free_space() < sizeof(pcm)) {
			svcSleepThread(10000000LL);   // 10 ms
			continue;
		}
		/* And never past the watermark: one decode call can read ahead a
		 * whole page, so the margin has to swallow the largest one. */
		__sync_synchronize();
		if (!ts->dl_done) {
			size_t off = (size_t)stb_vorbis_get_file_offset(v) +
			             SPOTIFY_OGG_HEADER;
			if (off + STREAM_MARGIN > ts->downloaded) {
				if (ts->dl_failed)
					break;
				svcSleepThread(20000000LL);   // 20 ms
				continue;
			}
			/* Walking back toward the span a far seek skipped: it is zero
			 * fill, and decoding it would end the song. Bring the download
			 * there (unless it is already on its way) and wait. */
			if (ts->has_hole && off + STREAM_MARGIN > ts->hole_lo &&
			    off < ts->hole_hi) {
				if (ts->dl_failed)
					break;
				if (ts->dl_restart_at < 0 && ts->downloaded > ts->hole_hi) {
					ts->dl_restart_at = (long)(ts->hole_lo & ~(size_t)15);
					nlog("SEEK: back into the gap, fetching it\n");
				}
				svcSleepThread(20000000LL);   // 20 ms
				continue;
			}
		}
		int got = stb_vorbis_get_samples_short_interleaved(v, 2, pcm, 4096 * 2);
		if (got <= 0)
			break;
		g.sink.write((const unsigned char *)pcm, (unsigned)got * 4);
		g.frames_out += (unsigned)got;
	}

	stb_vorbis_close(v);
	if (ts->dl_failed && !ts->dl_done) {
		snprintf(g.error, sizeof(g.error), "%s",
		         ts->dl_err[0] ? ts->dl_err : "the download failed halfway");
		g.state = NATIVE_FAILED;
	}
	nlog("PLAY fin: %u frames\n", g.frames_out);
}

/* A saved download is a complete file, so none of the streaming machinery
 * (watermarks, restart-at, the Spotify header) applies: read it whole and
 * decode straight through. Runs offline by construction. */
static void play_local(const char *name)
{
	char path[400];
	cfg_dir(path, sizeof(path));
	snprintf(path + strlen(path), sizeof(path) - strlen(path),
	         "music/%s", name);

	/* The decoder pulls from the card as it plays. Reading the whole file
	 * first held every saved song back for seconds, and the 8 MB block it
	 * needed was often not there beside a stream or a download: "could
	 * not read the file". */
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(g.error, sizeof(g.error), "file not found");
		g.state = NATIVE_FAILED;
		return;
	}
	setvbuf(f, NULL, _IOFBF, 32 * 1024);   /* few, large SD reads */

	int verr = 0;
	stb_vorbis *v = stb_vorbis_open_file(f, 1, &verr, NULL);   /* owns f */
	if (!v) {
		snprintf(g.error, sizeof(g.error),
		         "the decoder rejected the file (%d)", verr);
		g.state = NATIVE_FAILED;
		return;
	}

	/* The file is whole, so asking the decoder for the length is honest
	 * here (unlike the streaming path, where the tail is still zeroed). */
	g.total_frames = stb_vorbis_stream_length_in_samples(v);
	g.frames_out = 0;
	g.state = NATIVE_PLAYING;
	nlog("PLAY local %s\n", name);

	/* The cover saved beside the audio doubles as the offline artwork: a
	 * file: URL that artwork.c resolves on the SD instead of the CDN. */
	{
		char jpg[400];
		snprintf(jpg, sizeof(jpg), "%s", name);
		char *dot = strrchr(jpg, '.');
		if (dot)
			strcpy(dot, ".jpg");
		char full[520];
		cfg_dir(full, sizeof(full));
		snprintf(full + strlen(full), sizeof(full) - strlen(full),
		         "music/%s", jpg);
		struct stat st;
		if (stat(full, &st) == 0 && st.st_size > 0)
			snprintf(g.art, sizeof(g.art), "file:%s", jpg);
		else
			g.art[0] = '\0';
	}

	static s16 pcm[4096 * 2];
	while (!g.quit && !g.want_stop && !g.want_new) {
		log_tick(false);
		long want = g.want_seek_frame;
		if (want >= 0) {
			g.want_seek_frame = -1;
			__sync_synchronize();
			if ((unsigned)want >= g.total_frames)
				want = (long)g.total_frames - 1;
			if (stb_vorbis_seek(v, (unsigned)want)) {
				g.frames_out = (unsigned)want;
				nlog("SEEK local ok -> %ld\n", want);
			}
		}
		if (g.sink.free_space() < sizeof(pcm)) {
			svcSleepThread(10000000LL);   // 10 ms
			continue;
		}
		int got = stb_vorbis_get_samples_short_interleaved(v, 2, pcm,
		                                                   4096 * 2);
		if (got <= 0)
			break;
		g.sink.write((const unsigned char *)pcm, (unsigned)got * 4);
		g.frames_out += (unsigned)got;
	}

	stb_vorbis_close(v);
	nlog("PLAY local fin: %u frames\n", g.frames_out);
}

/* Rebuilds the access point session with the credentials kept from init.
 * The access token may have expired since, so one token refresh is tried
 * before giving up. Runs on the player thread, the only owner of g.ap. */
static bool ap_reconnect(void)
{
	char err[192] = "";
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(g.cands, g.cand_count, g.access_token,
	                              g.device_id, &info, &tried,
	                              err, sizeof(err));
	if (!ap) {
		char stored[512];
		spotify_tokens tok;
		char rerr[192] = "";
		if (!cfg_load_refresh_token(stored, sizeof(stored)) ||
		    token_refresh(stored, &tok, rerr, sizeof(rerr)) != 0) {
			nlog("AP: reconexion fallo: %s / %s\n", err, rerr);
			return false;
		}
		if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
			cfg_save_refresh_token(tok.refresh_token, tok.scope);
		snprintf(g.access_token, sizeof(g.access_token), "%s",
		         tok.access_token);
		ap = ap_login_any(g.cands, g.cand_count, g.access_token,
		                  g.device_id, &info, &tried, err, sizeof(err));
		if (!ap) {
			nlog("AP: reconexion fallo: %s\n", err);
			return false;
		}
	}
	g.ap = ap;
	nlog("AP: reconectado (%d candidatos)\n", tried);
	return true;
}

static void player_thread(void *arg)
{
	(void)arg;
	while (!g.quit) {
		log_tick(false);
		if (dlq_save_pending) {
			dlq_save_pending = false;
			dlq_save();
		}
		if (g.want_new) {
			char uri[192];
			LightLock_Lock(&g.lock);
			snprintf(uri, sizeof(uri), "%s", g.want);
			g.want_new = false;
			LightLock_Unlock(&g.lock);

			g.state = NATIVE_LOADING;
			g.error[0] = '\0';
			g.frames_out = g.total_frames = 0;
			g.want_seek_frame = -1;   /* a seek belongs to its track */

			/* A saved download: all local, no session to wait for. The
			 * display name is the filename, split on the " - " the saver
			 * writes. */
			if (strncmp(uri, "file:", 5) == 0) {
				nlog("FETCH local %s\n", uri + 5);
				char base[160];
				snprintf(base, sizeof(base), "%s", uri + 5);
				char *dot = strstr(base, ".ogg");
				if (dot)
					*dot = '\0';
				char *dash = strstr(base, " - ");
				if (dash) {
					*dash = '\0';
					snprintf(g.artist, sizeof(g.artist), "%s", base);
					snprintf(g.title, sizeof(g.title), "%s", dash + 3);
				} else {
					snprintf(g.title, sizeof(g.title), "%s", base);
					g.artist[0] = '\0';
				}
				g.album[0] = '\0';
				g.art[0] = '\0';
				g.album_uri[0] = g.artist_uri[0] = '\0';
				play_local(uri + 5);
				play_ended_at = osGetTime();
				if (g.state == NATIVE_PLAYING)
					g.state = NATIVE_IDLE;
				continue;
			}

			nlog("FETCH track %s\n", uri);
			/* Back-to-back playback never reaches the idle loop, so the
			 * token is also checked at every track change. */
			l5_refresh(false);

			/* The idle loop drops g.ap when the access point hangs up; the
			 * background reconnect below rebuilds it, so a track asked for
			 * right after a hiccup only has to wait for it. */
			if (!g.ap && !ap_reconnect()) {
				snprintf(g.error, sizeof(g.error),
				         "reconnecting to Spotify, try again");
				g.state = NATIVE_FAILED;
				nlog("  fallo: sin sesion AP\n");
				continue;
			}

			/* The stream lives in a heap job, not on this frame's stack:
			 * a skip detaches the still-running download instead of
			 * joining it, and the download thread owns the job from
			 * then on. */
			dl_job *job = calloc(1, sizeof(*job));
			if (!job) {
				snprintf(g.error, sizeof(g.error), "out of memory");
				g.state = NATIVE_FAILED;
				continue;
			}
			snprintf(job->uri, sizeof(job->uri), "%s", uri);
			char err[192];
			job->ts.sess = g.sp_sess;
			int rc = track_stream_begin(g.sp_host, l5_token(), g.ap,
			                            uri, NULL, &job->ts, err, sizeof(err));
			if (rc != 0 && strstr(err, "401")) {
				/* The library token ran out: renew it and try once more. */
				l5_refresh(true);
				job->ts.sess = g.sp_sess;
				rc = track_stream_begin(g.sp_host, l5_token(), g.ap, uri,
				                        NULL, &job->ts, err, sizeof(err));
			}
			if (rc == AP_KEY_REFUSED) {
				/* An account-side block (librespot#1649): reconnecting
				 * cannot help, it only adds seconds before the same no. */
				snprintf(err, sizeof(err), "%s", NATIVE_ERR_KEY_BLOCKED);
			} else if (rc != 0 &&
			    (strstr(err, "key request") || strstr(err, "audio key") ||
			     strstr(err, "connection"))) {
				/* Session-flavoured failure: the keepalive can still miss a
				 * quietly dead link, so drop it, rebuild once, and give the
				 * track one more chance before reporting anything. */
				nlog("  fallo de sesion (%s), reconectando y reintentando\n",
				     err);
				if (g.ap) {
					ap_close(g.ap);
					g.ap = NULL;
				}
				if (ap_reconnect()) {
					job->ts.sess = g.sp_sess;
					rc = track_stream_begin(g.sp_host, l5_token(),
					                        g.ap, uri, NULL, &job->ts, err,
					                        sizeof(err));
				}
			}
			if (rc != 0) {
				snprintf(g.error, sizeof(g.error), "%s", err);
				g.state = NATIVE_FAILED;
				nlog("  fallo: %s\n", err);
				free(job);
				continue;
			}
			snprintf(g.title, sizeof(g.title), "%s", job->ts.meta.name);
			snprintf(g.artist, sizeof(g.artist), "%s", job->ts.meta.artist);
			snprintf(g.album, sizeof(g.album), "%s", job->ts.meta.album);
			snprintf(g.art, sizeof(g.art), "%s", job->ts.meta.art_large);
			snprintf(g.album_uri, sizeof(g.album_uri), "%s",
			         job->ts.meta.album_uri);
			snprintf(g.artist_uri, sizeof(g.artist_uri), "%s",
			         job->ts.meta.artist_uri);

			/* The download runs beside the decoder. An arm that landed
			 * while the metadata fetch was still running has no job to
			 * stick to yet, so it is honoured here instead of dropped. */
			job->save = g.downloads_on || g.want_save;
			g.want_save = false;   /* the button arms per track */
			LightLock_Lock(&g.lock);
			job->next = g.jobs;
			g.jobs = job;
			g.job = job;
			LightLock_Unlock(&g.lock);
			const int worker = dl_reserve_wait();
			if (worker < 0) {
				snprintf(g.error, sizeof(g.error),
				         "could not start the download thread");
				g.state = NATIVE_FAILED;
				LightLock_Lock(&g.lock);
				g.jobs = job->next;
				g.job = NULL;
				LightLock_Unlock(&g.lock);
				track_stream_free(&job->ts);
				free(job);
				continue;
			}
			dl_run(worker, job);
			play_stream(&job->ts);
			play_ended_at = osGetTime();
			if (g.quit) {
				/* Shutting down: nothing outlives the player. */
				job->stop = true;
				while (job->handoff != 2)
					svcSleepThread(10000000LL);   // 10 ms
				LightLock_Lock(&g.lock);
				dl_job **p = &g.jobs;
				while (*p && *p != job)
					p = &(*p)->next;
				if (*p)
					*p = job->next;
				g.job = NULL;
				LightLock_Unlock(&g.lock);
				track_stream_free(&job->ts);
				free(job);
			} else {
				/* The download keeps filling in the background and saves
				 * itself if it was armed -- the next track starts now,
				 * not when this file lands. One background download at a
				 * time: an older one is told to stop, its RAM is dear.
				 * A stopped download that was armed for saving does not
				 * die: it waits its turn in the download queue. */
				char q_uri[64] = "", q_artist[128] = "", q_title[128] = "";
				LightLock_Lock(&g.lock);
				for (dl_job *p = g.jobs; p; p = p->next)
					if (p != job && p->handoff == 1 && !p->from_queue &&
					    !p->ts.dl_done && !p->ts.dl_failed) {
						p->stop = true;
						if (p->save && !p->saved && !q_uri[0]) {
							snprintf(q_uri, sizeof(q_uri), "%s",
							         p->uri);
							snprintf(q_artist, sizeof(q_artist), "%s",
							         p->ts.meta.artist);
							snprintf(q_title, sizeof(q_title), "%s",
							         p->ts.meta.name);
						}
					}
				g.job = NULL;
				LightLock_Unlock(&g.lock);
				if (q_uri[0])
					dlq_enqueue(q_uri, q_artist, q_title);
				job_detach(job);
			}
			if (g.state == NATIVE_PLAYING)
				g.state = NATIVE_IDLE;
			continue;
		}

		if (g.want_stop) {
			g.want_stop = false;
			g.state = NATIVE_IDLE;
		}

		if (g.offline) {
			/* No session to keep alive: local files only. */
			svcSleepThread(20000000LL);   // 20 ms
			continue;
		}

		/* Back from sleep: drop the dead links now, so the next song or
		 * list reconnects at once instead of waiting out a timeout. */
		if (g.woke) {
			g.woke = false;
			nlog("APT: woke up, reconnecting\n");
			if (g.ap) {
				ap_close(g.ap);
				g.ap = NULL;
			}
			http_session_close(g.sp_sess);
			g.sp_sess = http_session_open(g.sp_host);
		}

		/* The access point session is rebuilt in the background with a 5 s
		 * backoff: a dropped session used to end listening until the app
		 * restarted, and the console's wifi hiccups often enough for that
		 * to matter. */
		if (!g.ap) {
			static unsigned long long next_try = 0;
			unsigned long long now = osGetTime();
			if (now >= next_try) {
				ap_reconnect();
				next_try = osGetTime() + 5000;
			}
			svcSleepThread(20000000LL);   // 20 ms
			continue;
		}

		/* The queue starts its next download whenever the link is free;
		 * the download itself runs on its own thread. Not in the seconds
		 * after a song ends: starting one blocks this thread for a few
		 * seconds, and the UI is about to ask for the next song, which
		 * would then wait behind it (an audible gap at every handover). */
		/* A play request waits behind whatever this loop is doing, and
		 * one sat 9 s before it was picked up: say which step was slow. */
		u64 t_step = osGetTime();
		l5_refresh(false);
		if (osGetTime() - t_step > 1000)
			nlog("SLOW: token refresh %llu ms\n",
			     (unsigned long long)(osGetTime() - t_step));
		t_step = osGetTime();
		if (osGetTime() - play_ended_at > 10000)
			dlq_pump();
		if (osGetTime() - t_step > 1000)
			nlog("SLOW: queue start %llu ms\n",
			     (unsigned long long)(osGetTime() - t_step));

		/* Idle: keep the access point session alive. It only speaks to say
		 * ping, but a session that stops answering is dropped, and then the
		 * next audio key would fail for a reason that looks like anything
		 * but a missed ping. */
		if (ap_wait(g.ap, 200) == 1) {
			uint8_t buf[2048];
			uint8_t cmd;
			t_step = osGetTime();
			int n = ap_recv(g.ap, &cmd, buf, sizeof(buf));
			if (osGetTime() - t_step > 1000)
				nlog("SLOW: access point read %llu ms\n",
				     (unsigned long long)(osGetTime() - t_step));
			if (n == AP_PKT_TOO_BIG) {
				/* fine: nothing we need is that large */
			} else if (n < 0) {
				nlog("AP: sesion perdida mientras esperaba\n");
				ap_close(g.ap);
				g.ap = NULL;
			} else if (cmd == AP_PKT_PING) {
				ap_handle_ping(g.ap, buf, (size_t)n);
			}
		} else {
			svcSleepThread(20000000LL);   // 20 ms
		}
	}
}

int native_init(const native_sink *sink, void (*progress)(const char *step),
                char *err, size_t errlen)
{
	if (g.ready)
		return 0;
	if (!sink || !sink->write || !sink->free_space) {
		snprintf(err, errlen, "no audio sink");
		return -1;
	}
	g.sink = *sink;

	step(progress, "Starting TLS...");
	if (R_FAILED(psInit())) {
		snprintf(err, errlen, "no entropy source (ps:ps)");
		return -1;
	}
	int rc = tls_global_init();
	if (rc) {
		tls_strerror(rc, err, errlen);
		return -1;
	}
	tls_set_logger(write_log_line);

	step(progress, "Reading the stored token...");
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		snprintf(err, errlen, "not paired");
		return -1;
	}

	step(progress, "Refreshing the token...");
	spotify_tokens tok;
	if (token_refresh(stored, &tok, err, errlen) != 0)
		return -1;
	/* Spotify rotates it on every exchange, so the stored one is already dead
	 * by the time this returns. */
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	step(progress, "Finding an access point...");
	ap_candidate cands[AP_MAX_CANDIDATES];
	int n = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, errlen);
	if (n < 1)
		return -1;

	step(progress, "Signing in to the access point...");
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, n, tok.access_token, device_id, &info,
	                              &tried, err, errlen);
	if (!ap)
		return -1;
	snprintf(g.username, sizeof(g.username), "%s", info.username);
	/* Held, not closed: every audio key comes from this session. */
	g.ap = ap;
	/* Kept so a dropped session can be rebuilt without an app restart. */
	snprintf(g.access_token, sizeof(g.access_token), "%s", tok.access_token);
	snprintf(g.device_id, sizeof(g.device_id), "%s", device_id);
	memcpy(g.cands, cands, (size_t)n * sizeof(ap_candidate));
	g.cand_count = n;

	/* The avatar is not fetched here: the Web API rate-limits this shared
	 * client id often enough (HTTP 429) that a blocking call at init mostly
	 * slowed the sign-in down to still come back empty. The cached URL loads
	 * instead, and the art worker makes exactly one background attempt via
	 * native_profile_art_refresh() when there is none. */
	cfg_load_profile_art(g.profile_art, sizeof(g.profile_art));

	step(progress, "Getting the library token...");
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &g.l5, err, errlen) != 0)
		return -1;
	memcpy(g.l5_cred, info.reusable, info.reusable_len);
	g.l5_cred_len = info.reusable_len;
	l5_publish();

	char ignored[128];
	sp_resolve(g.sp_host, sizeof(g.sp_host), ignored, sizeof(ignored));
	g.sp_sess = http_session_open(g.sp_host);   /* connects lazily */

	LightLock_Init(&g.lock);
	LightLock_Init(&ids_lock);
	LightLock_Init(&l5_lock);
	g.want_seek_frame = -1;
	g.state = NATIVE_IDLE;
	g.ready = true;
	dlq_load();
	/* track_stream_begin() handshakes TLS on this thread; 64 KB overflowed
	 * on unlucky cert chains (two crash dumps died mid-handshake). */
	g.player = threadCreate(player_thread, NULL, 128 * 1024, 0x30, -2, false);
	if (!g.player) {
		snprintf(err, errlen, "could not start the player thread");
		g.ready = false;
		return -1;
	}
	nlog("INIT: ok, %s via %s\n", g.username, g.sp_host);
	mem_note("start");
	return 0;
}

int native_init_offline(const native_sink *sink)
{
	if (g.ready)
		return 0;
	if (!sink || !sink->write || !sink->free_space)
		return -1;
	g.sink = *sink;
	g.offline = true;
	snprintf(g.username, sizeof(g.username), "Offline");
	LightLock_Init(&g.lock);
	LightLock_Init(&ids_lock);
	g.want_seek_frame = -1;
	g.state = NATIVE_IDLE;
	g.ready = true;
	/* No TLS on this thread in offline mode, but the same stack keeps the
	 * player code identical in both modes. */
	g.player = threadCreate(player_thread, NULL, 128 * 1024, 0x30, -2, false);
	if (!g.player) {
		g.ready = false;
		return -1;
	}
	nlog("INIT: modo offline\n");
	return 0;
}

/* ---- Casting to a speaker ------------------------------------------------
 *
 * The speakers are listed in cast.txt beside the config, one per line
 * ("Kitchen 192.168.1.20": the name as the Google Home app shows it, then
 * its address); START picks among them. The work is
 * network and seconds long, so it runs on its own thread; the UI only
 * posts requests and reads the state. Spotify on the speaker does the
 * streaming, so the console's own player just sits paused meanwhile. */

enum { CR_PLAY = 1, CR_CMD, CR_VOL, CR_END, CR_RESUME, CR_FIND };
#define CAST_REQS 8

#define CAST_MAX 12

static struct {
	bool       loaded, configured;
	char       name[64], ip[48];  /* the speaker picked now */
	char       names[CAST_MAX][64], ips[CAST_MAX][48];   /* cast.txt */
	int        n, cur;
	volatile bool finding;        /* a search for speakers is running */
	volatile int  found;          /* what the last one found, -1 failed */
	Thread     th;
	LightEvent wake;
	volatile native_cast_status state;
	char       dev[CAST_DEV_LEN];
	bool       awake;             /* signed in; a failed command re-wakes */
	volatile bool relink;         /* the console slept: reopen the link */
	volatile int gen;             /* bumped by every "back to the console" */
	float      volume;            /* the speaker's level, 0..1 */
	char       error[160];
	char       last_ctx[192], last_track[64];   /* the last play that went */
	struct {
		int  kind;
		char a[192], b[64];   /* play: context and track; cmd: endpoint */
		long n;               /* play: position in ms; vol: step */
	} req[CAST_REQS];
	int        head, count;
} cast;

static void cast_load(void)
{
	if (cast.loaded)
		return;
	cast.loaded = true;
	char path[320];
	cfg_dir(path, sizeof(path));
	strncat(path, "cast.txt", sizeof(path) - strlen(path) - 1);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	/* One speaker per line: its Cast name, then its address. */
	char line[160];
	while (cast.n < CAST_MAX && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#')
			continue;
		/* The address is the last word; the name may have spaces. */
		char *sp = strrchr(line, ' ');
		if (!sp)
			continue;
		*sp = '\0';
		const char *ip = sp + 1 + strspn(sp + 1, " ");
		while (sp > line && sp[-1] == ' ')
			*--sp = '\0';
		if (!line[0] || !ip[0])
			continue;
		snprintf(cast.names[cast.n], sizeof(cast.names[0]), "%s", line);
		snprintf(cast.ips[cast.n], sizeof(cast.ips[0]), "%s", ip);
		nlog("CAST: speaker %s at %s\n", line, ip);
		cast.n++;
	}
	fclose(f);
	cast.configured = cast.n > 0;
	/* The speaker used last time, by name. */
	cfg_dir(path, sizeof(path));
	strncat(path, "cast_last.txt", sizeof(path) - strlen(path) - 1);
	f = fopen(path, "r");
	if (f) {
		if (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			for (int i = 0; i < cast.n; i++)
				if (strcmp(cast.names[i], line) == 0)
					cast.cur = i;
		}
		fclose(f);
	}
	if (cast.configured) {
		snprintf(cast.name, sizeof(cast.name), "%s", cast.names[cast.cur]);
		snprintf(cast.ip, sizeof(cast.ip), "%s", cast.ips[cast.cur]);
	}
}

int native_cast_count(void)
{
	cast_load();
	return cast.n;
}

int native_cast_current(void) { return cast.cur; }

/* Picks speaker `i` of cast.txt. Only while not casting: the one playing
 * stays the one the remote talks to. */
void native_cast_pick(int i)
{
	cast_load();
	if (i < 0 || i >= cast.n || cast.state == NATIVE_CAST_STARTING ||
	    cast.state == NATIVE_CAST_ON)
		return;
	LightLock_Lock(&g.lock);
	cast.cur = i;
	snprintf(cast.name, sizeof(cast.name), "%s", cast.names[cast.cur]);
	snprintf(cast.ip, sizeof(cast.ip), "%s", cast.ips[cast.cur]);
	cast.dev[0] = '\0';
	cast.awake = false;
	cast.last_ctx[0] = cast.last_track[0] = '\0';
	LightLock_Unlock(&g.lock);
	char path[320];
	cfg_dir(path, sizeof(path));
	strncat(path, "cast_last.txt", sizeof(path) - strlen(path) - 1);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", cast.name);
		fclose(f);
	}
}

bool native_cast_configured(void)
{
	cast_load();
	return cast.configured;
}

const char *native_cast_name(void) { return cast.name; }
native_cast_status native_cast_state(void) { return cast.state; }
const char *native_cast_error(void) { return cast.error; }

/* Wakes the speaker and signs it in; on success it is a Connect device
 * until Spotify forgets it. */
static bool cast_do_wake(void)
{
	char err[160] = "";
	float vol = -1.0f;
	const u64 t0 = osGetTime();
	int rc = cast_wake(cast.ip, cast.name, g.sp_host, l5_token(),
	                   g.access_token, cast.dev, &vol, err, sizeof(err));
	if (rc != 0 && strstr(err, "401")) {
		l5_refresh(true);
		rc = cast_wake(cast.ip, cast.name, g.sp_host, l5_token(),
		               g.access_token, cast.dev, &vol, err, sizeof(err));
	}
	if (rc != 0) {
		nlog("CAST: waking %s failed: %s\n", cast.name, err);
		snprintf(cast.error, sizeof(cast.error), "%s", err);
		cast.awake = false;
		return false;
	}
	cast.awake = true;
	if (vol >= 0.0f)
		cast.volume = vol;
	nlog("CAST: %s signed in as %s in %llu ms\n", cast.name, cast.dev,
	     (unsigned long long)(osGetTime() - t0));
	return true;
}

/* Writes the speaker list back, comments first so a hand-edited file
 * still explains itself. */
static void cast_save_list(void)
{
	char path[320], tmp[328];
	cfg_dir(path, sizeof(path));
	strncat(path, "cast.txt", sizeof(path) - strlen(path) - 1);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "# Speakers for START > Play on: the name as the Google Home "
	           "app shows it,\n# then its IP address. START > Find "
	           "speakers rewrites this list.\n");
	LightLock_Lock(&g.lock);
	for (int i = 0; i < cast.n; i++)
		fprintf(f, "%s %s\n", cast.names[i], cast.ips[i]);
	LightLock_Unlock(&g.lock);
	if (fclose(f) == 0) {
		remove(path);
		rename(tmp, path);
	} else {
		remove(tmp);
	}
}

/* Asks the network for speakers and folds them into the list: a known name
 * gets its new address, a new one is added, and a speaker that did not
 * answer (switched off, or typed in by hand) stays. */
static void cast_find(void)
{
	static cast_found hits[CAST_MAX];
	char err[96] = "";
	const u64 t0 = osGetTime();
	const int n = cast_discover(hits, CAST_MAX, 3000, err, sizeof(err));
	if (n < 0) {
		nlog("CAST: finding speakers failed: %s\n", err);
		cast.found = -1;
		return;
	}
	LightLock_Lock(&g.lock);
	for (int i = 0; i < n; i++) {
		int at = -1;
		for (int j = 0; j < cast.n; j++)
			if (strcmp(cast.names[j], hits[i].name) == 0)
				at = j;
		if (at < 0 && cast.n < CAST_MAX)
			at = cast.n++;
		if (at < 0)
			continue;
		snprintf(cast.names[at], sizeof(cast.names[0]), "%s", hits[i].name);
		snprintf(cast.ips[at], sizeof(cast.ips[0]), "%s", hits[i].ip);
	}
	cast.configured = cast.n > 0;
	if (cast.cur >= cast.n)
		cast.cur = 0;
	if (cast.configured) {
		snprintf(cast.name, sizeof(cast.name), "%s", cast.names[cast.cur]);
		snprintf(cast.ip, sizeof(cast.ip), "%s", cast.ips[cast.cur]);
	}
	LightLock_Unlock(&g.lock);
	for (int i = 0; i < n; i++)
		nlog("CAST: found %s (%s) at %s\n", hits[i].name, hits[i].model,
		     hits[i].ip);
	nlog("CAST: %d speakers answered in %llu ms (%s)\n", n,
	     (unsigned long long)(osGetTime() - t0), err);
	if (n > 0)
		cast_save_list();
	cast.found = n;
}

/* The Spotify id of a saved song, for casting it: from ids.txt, or, for a
 * file saved before that list existed, from a search for its name. A hit
 * counts only when it would save under the very same file name. */
static bool cast_file_uri(const char *file, char *uri, size_t urilen)
{
	const char *name = file + 5;
	if (file_id_find(name, uri, urilen))
		return true;
	char q[200];
	snprintf(q, sizeof(q), "%s", name);
	char *dot = strrchr(q, '.');
	if (dot)
		*dot = '\0';
	char *dash = strstr(q, " - ");
	if (dash)
		memmove(dash + 1, dash + 3, strlen(dash + 3) + 1);
	static char hits[5][64];
	static sp_track meta[5];
	char err[160] = "";
	int n = sp_search_tracks(g.sp_host, l5_token(), q, hits, 5, err,
	                         sizeof(err));
	if (n <= 0) {
		nlog("CAST: search for %s: %s\n", q, n < 0 ? err : "nothing");
		return false;
	}
	const char *ptrs[5];
	for (int i = 0; i < n; i++)
		ptrs[i] = hits[i];
	memset(meta, 0, sizeof(meta));
	if (sp_track_metadata_batch(g.sp_host, l5_token(), ptrs, n, meta, err,
	                            sizeof(err)) < 0) {
		nlog("CAST: naming the hits for %s: %s\n", q, err);
		return false;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < n; i++) {
			if (!meta[i].name[0] || !meta[i].uri[0])
				continue;
			char got[160];
			download_name(meta[i].artist, meta[i].name, got, sizeof(got));
			if (pass == 0 ? strcmp(got, name) != 0
			              : strcasecmp(got, name) != 0)
				continue;
			snprintf(uri, urilen, "%s", meta[i].uri);
			file_id_note(name, uri);
			nlog("CAST: %s is %s\n", name, uri);
			return true;
		}
	}
	nlog("CAST: no search hit saves as %s\n", name);
	return false;
}

/* A command that finds the speaker gone (Spotify signs idle ones out)
 * wakes it again once and retries. */
static int cast_send_cmd(int kind, const char *a, const char *b, long n)
{
	char err[160] = "";
	for (int attempt = 0; attempt < 2; attempt++) {
		int st;
		if (kind == CR_PLAY) {
			st = cast_play(g.sp_host, l5_token(), cast.dev, a, b, n, err,
			               sizeof(err));
		} else if (kind == CR_VOL) {
			st = cast_set_volume(g.sp_host, l5_token(), cast.dev,
			                     cast.volume, err, sizeof(err));
		} else {
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "{\"endpoint\":\"%s\"}", a);
			st = cast_command(g.sp_host, l5_token(), cast.dev, cmd, err,
			                  sizeof(err));
		}
		if (st == 200)
			return 0;
		nlog("CAST: %s: %s\n", kind == CR_PLAY ? "play" : a, err);
		if (attempt == 0 && st == 401) {
			l5_refresh(true);   /* our token, not the speaker */
			continue;
		}
		if (attempt == 0 && !cast_do_wake())
			break;
	}
	snprintf(cast.error, sizeof(cast.error), "%s", err);
	return -1;
}

static void cast_thread(void *arg)
{
	(void)arg;
	/* One link to spclient for every command: pause should feel like a
	 * button, not a handshake. It connects on first use. */
	http_session *sess = http_session_open(g.sp_host);
	cast_use_session(sess);
	while (!g.quit) {
		LightEvent_WaitTimeout(&cast.wake, 500000000LL);   // 0.5 s
		/* The speaker is driven with the console's token: keep it fresh
		 * here, the player may sit paused for hours under a cast. */
		if (cast.state != NATIVE_CAST_OFF)
			l5_refresh(false);
		if (cast.relink) {
			cast.relink = false;
			cast_use_session(NULL);
			http_session_close(sess);
			sess = http_session_open(g.sp_host);
			cast_use_session(sess);
		}
		for (;;) {
			int kind;
			char a[192], b[64];
			long n;
			LightLock_Lock(&g.lock);
			if (!cast.count) {
				LightLock_Unlock(&g.lock);
				break;
			}
			const int i = cast.head;
			kind = cast.req[i].kind;
			memcpy(a, cast.req[i].a, sizeof(a));
			memcpy(b, cast.req[i].b, sizeof(b));
			n = cast.req[i].n;
			cast.head = (cast.head + 1) % CAST_REQS;
			cast.count--;
			LightLock_Unlock(&g.lock);
			if (g.quit)
				break;

			if (kind == CR_PLAY) {
				/* A "back to the console" posted while this starts must
				 * not be undone by it turning ON afterwards. */
				const int gen = cast.gen;
				cast.error[0] = '\0';
				if (strncmp(a, "file:", 5) == 0) {
					char uri[64];
					if (!cast_file_uri(a, uri, sizeof(uri))) {
						snprintf(cast.error, sizeof(cast.error),
						         "no Spotify id for %s", a + 5);
						if (gen == cast.gen)
							cast.state = NATIVE_CAST_FAILED;
						continue;
					}
					snprintf(a, sizeof(a), "%s", uri);
				}
				if ((!cast.awake && !cast_do_wake()) ||
				    cast_send_cmd(CR_PLAY, a, b, n) != 0) {
					if (gen == cast.gen)
						cast.state = NATIVE_CAST_FAILED;
					continue;
				}
				nlog("CAST: playing %s on %s\n", a, cast.name);
				snprintf(cast.last_ctx, sizeof(cast.last_ctx), "%s", a);
				snprintf(cast.last_track, sizeof(cast.last_track), "%s", b);
				if (gen == cast.gen)
					cast.state = NATIVE_CAST_ON;
			} else if (kind == CR_RESUME) {
				/* Back after a drop (a sleep, a lost link): the speaker
				 * may still be playing, or signed out and blank, in which
				 * case the last list starts over. */
				const int gen = cast.gen;
				cast.error[0] = '\0';
				bool ok = cast_send_cmd(CR_CMD, "resume", "", 0) == 0;
				if (!ok && cast.awake && cast.last_ctx[0])
					ok = cast_send_cmd(CR_PLAY, cast.last_ctx,
					                   cast.last_track, 0) == 0;
				nlog("CAST: reconnecting to %s %s\n", cast.name,
				     ok ? "worked" : "failed");
				if (gen == cast.gen)
					cast.state = ok ? NATIVE_CAST_ON : NATIVE_CAST_FAILED;
			} else if (kind == CR_FIND) {
				cast_find();
				cast.finding = false;
			} else if (kind == CR_END) {
				/* Back to the console: the speaker stops. */
				if (cast.dev[0])
					cast_send_cmd(CR_CMD, "pause", "", 0);
				cast.state = NATIVE_CAST_OFF;
			} else if (cast.state == NATIVE_CAST_ON) {
				if (kind == CR_VOL) {
					cast.volume += n * 0.05f;
					if (cast.volume < 0.0f) cast.volume = 0.0f;
					if (cast.volume > 1.0f) cast.volume = 1.0f;
				}
				/* Even a re-wake did not get through: the link is gone,
				 * and the UI offers to reconnect. */
				if (cast_send_cmd(kind, a, b, n) != 0 &&
				    cast.state == NATIVE_CAST_ON)
					cast.state = NATIVE_CAST_FAILED;
			}
		}
	}
	cast_use_session(NULL);
	http_session_close(sess);
}

static void cast_post(int kind, const char *a, const char *b, long n)
{
	/* Finding speakers is how a cast.txt comes to exist. */
	if (!g.ready || (kind != CR_FIND && !native_cast_configured()))
		return;
	LightLock_Lock(&g.lock);
	if (!cast.th) {
		LightEvent_Init(&cast.wake, RESET_ONESHOT);
		cast.volume = 0.5f;
		/* The TLS handshake runs on this stack, like the downloads'. */
		/* Above the download workers: a pause should not queue behind
		 * a song's decryption. */
		cast.th = native_spawn_worker(cast_thread, NULL, 96 * 1024, 0x30);
		if (!cast.th) {
			LightLock_Unlock(&g.lock);
			snprintf(cast.error, sizeof(cast.error), "no thread for casting");
			cast.state = NATIVE_CAST_FAILED;
			mem_note("no cast thread");
			return;
		}
	}
	if (cast.count == CAST_REQS) {   /* full: the oldest gives way */
		cast.head = (cast.head + 1) % CAST_REQS;
		cast.count--;
	}
	const int i = (cast.head + cast.count) % CAST_REQS;
	cast.req[i].kind = kind;
	snprintf(cast.req[i].a, sizeof(cast.req[i].a), "%s", a ? a : "");
	snprintf(cast.req[i].b, sizeof(cast.req[i].b), "%s", b ? b : "");
	cast.req[i].n = n;
	cast.count++;
	LightLock_Unlock(&g.lock);
	LightEvent_Signal(&cast.wake);
}

void native_cast_play(const char *context_uri, const char *track_uri,
                      long pos_ms)
{
	cast.state = NATIVE_CAST_STARTING;   /* the UI shows it at once */
	cast_post(CR_PLAY, context_uri, track_uri, pos_ms);
}

void native_cast_command(const char *endpoint)
{
	cast_post(CR_CMD, endpoint, "", 0);
}

void native_cast_volume_step(int dir)
{
	cast_post(CR_VOL, "", "", dir);
}

void native_cast_find(void)
{
	cast_load();
	if (cast.finding || g.offline || cast.state == NATIVE_CAST_STARTING ||
	    cast.state == NATIVE_CAST_ON)
		return;
	cast.finding = true;
	cast_post(CR_FIND, "", "", 0);
	if (!cast.th)
		cast.finding = false;   /* no thread: nothing will run it */
}

bool native_cast_finding(void) { return cast.finding; }
int  native_cast_found(void) { return cast.found; }

void native_on_wake(void)
{
	g.woke = true;
	cast.relink = true;
}

void native_cast_reconnect(void)
{
	cast.state = NATIVE_CAST_STARTING;
	cast_post(CR_RESUME, "", "", 0);
}

void native_cast_end(void)
{
	cast.gen++;
	cast_post(CR_END, "", "", 0);
	cast.state = NATIVE_CAST_OFF;
}

int native_cast_volume(void)
{
	return (int)(cast.volume * 100.0f + 0.5f);
}

/* Tells every backend thread to wrap up, without waiting: the UI calls it
 * first, so they stop side by side while it joins its own threads, rather
 * than one after another inside native_exit. */
void native_quit_signal(void)
{
	if (!g.ready || g.quit)
		return;
	nlog("EXIT: begin\n");
	g.quit = true;
	/* Every download, the queue's included, is told to stop. A detached
	 * one frees itself; the teardown below must not run under it. */
	LightLock_Lock(&g.lock);
	for (dl_job *p = g.jobs; p; p = p->next)
		p->stop = true;
	LightLock_Unlock(&g.lock);
	if (cast.th)
		LightEvent_Signal(&cast.wake);
	for (int i = 0; i < DL_WORKERS; i++)
		if (dlw[i].th)
			LightEvent_Signal(&dlw[i].go);
}

/* threadJoin, with a log line when it had to wait. */
static bool join_noted(Thread t, u64 ns, const char *what)
{
	const u64 t0 = osGetTime();
	const bool ok = R_SUCCEEDED(threadJoin(t, ns));
	const u64 ms = osGetTime() - t0;
	if (ms > 200 || !ok)
		nlog("EXIT: %s thread %s after %llu ms\n", what,
		     ok ? "done" : "STILL RUNNING", (unsigned long long)ms);
	return ok;
}

void native_exit(void)
{
	if (!g.ready)
		return;
	const u64 t_exit = osGetTime();
	native_quit_signal();

	bool cast_done = true;
	if (cast.th) {
		LightEvent_Signal(&cast.wake);
		cast_done = join_noted(cast.th, 3000000000ULL, "cast");
		if (cast_done)
			threadFree(cast.th);
		cast.th = NULL;
	}
	for (int i = 0; i < DL_WORKERS; i++)
		if (dlw[i].th) {
			LightEvent_Signal(&dlw[i].go);
			if (join_noted(dlw[i].th, 3000000000ULL, "download"))
				threadFree(dlw[i].th);
			else
				cast_done = false;   /* still in a socket: no teardown */
			dlw[i].th = NULL;
		}
	bool player_done = cast_done;
	if (g.player) {
		player_done = join_noted(g.player, 6000000000ULL, "player");
		if (player_done)
			threadFree(g.player);
	}
	/* A stop is only noticed when the next bytes arrive, so give the
	 * stragglers a few seconds to drain out of the list. */
	bool jobs_done = false;
	for (int i = 0; i < 60 && !jobs_done; i++) {
		LightLock_Lock(&g.lock);
		jobs_done = g.jobs == NULL;
		LightLock_Unlock(&g.lock);
		if (!jobs_done)
			svcSleepThread(100000000LL);   // 100 ms
	}
	if (!player_done || !jobs_done) {
		/* Something is still blocked in a socket. Freeing the state it
		 * uses is what crashed on quit; leaving it alone costs nothing,
		 * the process is about to end and the kernel reclaims it all. */
		nlog("EXIT: workers still running, skipping teardown\n");
		log_tick(true);
		return;
	}
	nlog("EXIT: backend down in %llu ms\n",
	     (unsigned long long)(osGetTime() - t_exit));
	log_tick(true);
	if (g.ap)
		ap_close(g.ap);
	http_session_close(g.sp_sess);
	memset(&g, 0, sizeof(g));
	tls_global_exit();
	psExit();
}

/* Turns a list of track URIs into rows, naming them in one batch: on this
 * console the cost is the round trips, so a screenful must cost one. */
static int name_tracks(char uris[][64], int n, native_row *rows, int max,
                       char *err, size_t errlen)
{
	nlog("  naming %d uris in one batch\n", n);
	if (n > max)
		n = max;
	if (n <= 0)
		return 0;

	/* The metadata service happily answers ~50 tracks per call; a 500-song
	 * liked list in one shot risks a rejected or truncated reply, so chunk
	 * and accumulate. A failed chunk is skipped, not fatal: the rows stay
	 * blank-labelled rather than losing the whole list. */
	static const char *ptrs[50];
	static sp_track meta[50];
	/* The chunks of one list go over a single keep-alive connection: each
	 * costs ~3 s with its own handshake and well under one over an open
	 * link. NULL session = the old one-shot path, still correct. */
	http_session *sess = http_session_open(g.sp_host);
	int done = 0;
	for (int base = 0; base < n; base += 50) {
		if (fetch_cancel) {
			snprintf(err, errlen, "cancelled");
			nlog("  naming cancelado en el lote %d\n", base);
			http_session_close(sess);
			return -1;
		}
		int chunk = n - base > 50 ? 50 : n - base;
		for (int i = 0; i < chunk; i++)
			ptrs[i] = uris[base + i];
		TickCounter t;
		osTickCounterStart(&t);
		int named = sess
		    ? sp_track_metadata_batch_s(sess, l5_token(), ptrs,
		                                chunk, meta, err, errlen)
		    : sp_track_metadata_batch(g.sp_host, l5_token(), ptrs,
		                              chunk, meta, err, errlen);
		osTickCounterUpdate(&t);
		unsigned w_ms = 0, b_ms = 0;
		http_session_last_timing(sess, &w_ms, &b_ms);
		nlog("  chunk %d..%d -> %d named%s%s (%.0f ms%s, server %u, body %u)\n",
		     base, base + chunk - 1, named, named < 0 ? ": " : "",
		     named < 0 ? err : "", osTickCounterRead(&t),
		     sess && http_session_last_reused(sess) ? ", ka" : "", w_ms, b_ms);
		if (named < 0)
			continue;
		/* Answers are matched to rows by uri, not position: a list that
		 * holds a song twice can get one answer for both, and a positional
		 * fill would then shift every later name onto the wrong row. */
		int next = base;
		for (int i = 0; i < named; i++) {
			int j = base + i;
			if (meta[i].uri[0]) {
				for (int k = next; k < base + chunk; k++)
					if (!rows[k].uri[0] && strcmp(uris[k], meta[i].uri) == 0) {
						j = k;
						break;
					}
			}
			if (rows[j].uri[0])
				continue;
			if (j == next)
				next++;
			native_row *r = &rows[j];
			snprintf(r->label, sizeof(r->label), "%s",
			         meta[i].name[0] ? meta[i].name : "(untitled)");
			snprintf(r->sub, sizeof(r->sub), "%s", meta[i].artist);
			snprintf(r->uri, sizeof(r->uri), "%s",
			         meta[i].uri[0] ? meta[i].uri : uris[j]);
			snprintf(r->art, sizeof(r->art), "%s", meta[i].art_small);
			snprintf(r->album, sizeof(r->album), "%s", meta[i].album);
			snprintf(r->artist_uri, sizeof(r->artist_uri), "%s",
			         meta[i].artist_uri);
			snprintf(r->album_uri, sizeof(r->album_uri), "%s",
			         meta[i].album_uri);
			r->is_playlist = false;
		}
		/* A repeat that got no answer of its own borrows its twin's. */
		for (int k = base; k < base + chunk; k++) {
			if (rows[k].uri[0])
				continue;
			for (int m = base; m < base + chunk; m++)
				if (rows[m].uri[0] && strcmp(uris[m], uris[k]) == 0) {
					rows[k] = rows[m];
					break;
				}
		}
		if (named > done - base)
			done = base + named;
		if (progress_cb)
			progress_cb(rows, done, n);
	}
	http_session_close(sess);
	if (done <= 0)
		return -1;
	/* Rows past a partially-failed chunk still need their uri so they can
	 * play; the label falls back to a placeholder. */
	for (int i = 0; i < n; i++) {
		if (rows[i].uri[0])
			continue;
		snprintf(rows[i].uri, sizeof(rows[i].uri), "%s", uris[i]);
		snprintf(rows[i].label, sizeof(rows[i].label), "(untitled)");
		rows[i].is_playlist = false;
	}
	return n;
}

/* Lists the .ogg files under music/ as playable rows, newest first (the
 * order the saver produced them in is the order the user last heard).
 * Pure filesystem: no session, no network. */
int native_playlist_add(const char *playlist_uri, const char *track_uri,
                        char *err, size_t errlen)
{
	if (!g.ready || g.offline) {
		snprintf(err, errlen, "offline");
		return -1;
	}
	int rc = sp_playlist_add(g.sp_host, l5_token(), playlist_uri, track_uri,
	                         err, errlen);
	nlog("ADD: %s -> %s: %s\n", track_uri, playlist_uri, rc == 0 ? "ok" : err);
	return rc;
}

int native_playlists_cached(native_row *rows, int max)
{
	return rootlist_cache_load(rows, max);
}

bool native_delete_download(const char *file_uri)
{
	if (!file_uri || strncmp(file_uri, "file:", 5) != 0)
		return false;
	const char *name = file_uri + 5;
	/* Only a bare file name under music/: nothing that walks elsewhere. */
	if (!name[0] || strchr(name, '/') || strchr(name, '\\') ||
	    strstr(name, ".."))
		return false;
	char path[400];
	cfg_dir(path, sizeof(path));
	snprintf(path + strlen(path), sizeof(path) - strlen(path), "music/%s", name);
	bool ok = remove(path) == 0;
	/* Its cover goes with it: same name, .jpg. */
	char *dot = strrchr(path, '.');
	if (dot && strcmp(dot, ".ogg") == 0) {
		strcpy(dot, ".jpg");
		remove(path);
	}
	saved_checked_uri[0] = '\0';   /* the "saved?" cache must look again */
	nlog("DL: deleted %s%s\n", name, ok ? "" : " (failed)");
	return ok;
}

/* One saved song as a row: label from the file name, size as subtitle,
 * the <name>.jpg beside it (if any) as its thumbnail. */
static void download_row(const char *dir, const char *file, native_row *r)
{
	const size_t len = strlen(file);
	memset(r, 0, sizeof(*r));
	snprintf(r->uri, sizeof(r->uri), "file:%s", file);
	snprintf(r->label, sizeof(r->label), "%.*s", (int)len - 4, file);
	struct stat st;
	char path[400];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if (stat(path, &st) == 0)
		snprintf(r->sub, sizeof(r->sub), "%.1f MB",
		         (double)st.st_size / (1024.0 * 1024.0));
	char jpg[200];
	snprintf(jpg, sizeof(jpg), "%.*s.jpg", (int)len - 4, file);
	snprintf(path, sizeof(path), "%s/%s", dir, jpg);
	if (stat(path, &st) == 0 && st.st_size > 0)
		snprintf(r->art, sizeof(r->art), "file:%s", jpg);
}

static bool file_exists(const char *dir, const char *file)
{
	char path[400];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	struct stat st;
	return stat(path, &st) == 0 && st.st_size > 0;
}

/* Saved playlists: music/lists/<name>.txt, one song file name per line,
 * written when a list is queued. The songs themselves stay in music/, once
 * each, however many lists they belong to. */
static void dlist_dir(char *out, size_t n)
{
	cfg_dir(out, n);
	strncat(out, "music/lists", n - strlen(out) - 1);
}

static void dlist_write(const char *title, const native_row *rows, int n)
{
	char dir[352], music[352];
	cfg_dir(music, sizeof(music));
	strncat(music, "music", sizeof(music) - strlen(music) - 1);
	mkdir(music, 0777);
	dlist_dir(dir, sizeof(dir));
	mkdir(dir, 0777);
	char safe[64];
	int k = name_part(safe, 0, title && title[0] ? title : "Playlist", 60);
	safe[k] = '\0';
	char path[440];
	snprintf(path, sizeof(path), "%s/%s.txt", dir, safe);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++) {
		if (rows[i].is_playlist ||
		    strncmp(rows[i].uri, "spotify:track:", 14) != 0)
			continue;
		char name[160];
		download_name(rows[i].sub, rows[i].label, name, sizeof(name));
		fprintf(f, "%s\n", name);
	}
	fclose(f);
}

/* 64-bit FNV-1a of a file name: the top of Downloads remembers which songs
 * a saved list already shows, without keeping every name around. */
static uint64_t name_hash(const char *s)
{
	uint64_t h = 1469598103934665603ULL;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 1099511628211ULL;
	return h;
}

static int fetch_downloads(const char *arg, native_row *rows, int max)
{
	char dir[352];
	cfg_dir(dir, sizeof(dir));
	strncat(dir, "music", sizeof(dir) - strlen(dir) - 1);
	char ldir[352];
	dlist_dir(ldir, sizeof(ldir));
	int n = 0;

	/* Inside one saved playlist: its songs, in list order, if on the card. */
	if (arg && strncmp(arg, "dlist:", 6) == 0) {
		char path[440];
		snprintf(path, sizeof(path), "%s/%s", ldir, arg + 6);
		FILE *f = fopen(path, "r");
		if (!f)
			return 0;
		char line[200];
		while (n < max && fgets(line, sizeof(line), f)) {
			char *nl = strpbrk(line, "\r\n");
			if (nl)
				*nl = '\0';
			if (line[0] && file_exists(dir, line))
				download_row(dir, line, &rows[n++]);
		}
		fclose(f);
		nlog("FETCH downloads %s -> %d\n", arg + 6, n);
		return n;
	}

	/* The top: one folder per saved playlist or album, then the songs saved
	 * on their own. A song that belongs to a saved list shows inside that
	 * list only (the file itself stays in music/, once, however many lists
	 * hold it), so a downloaded album does not spill over the top. */
	uint64_t *in_list = NULL;
	int in_n = 0, in_cap = 0;
	DIR *ld = opendir(ldir);
	if (ld) {
		struct dirent *e;
		while (n < max && (e = readdir(ld))) {
			const size_t len = strlen(e->d_name);
			if (len < 5 || strcasecmp(e->d_name + len - 4, ".txt") != 0)
				continue;
			char path[440];
			snprintf(path, sizeof(path), "%s/%s", ldir, e->d_name);
			FILE *f = fopen(path, "r");
			if (!f)
				continue;
			int have = 0;
			char first[200] = "", line[200];
			while (fgets(line, sizeof(line), f)) {
				char *nl = strpbrk(line, "\r\n");
				if (nl)
					*nl = '\0';
				if (line[0] && file_exists(dir, line)) {
					if (!have)
						snprintf(first, sizeof(first), "%s", line);
					have++;
					if (in_n == in_cap) {
						int cap = in_cap ? in_cap * 2 : 256;
						uint64_t *grown = realloc(in_list,
						                          (size_t)cap * sizeof(*grown));
						if (grown) {
							in_list = grown;
							in_cap = cap;
						}
					}
					if (in_n < in_cap)
						in_list[in_n++] = name_hash(line);
				}
			}
			fclose(f);
			if (!have)
				continue;   /* nothing of it saved (yet): no empty folder */
			native_row *r = &rows[n++];
			memset(r, 0, sizeof(*r));
			snprintf(r->uri, sizeof(r->uri), "dlist:%s", e->d_name);
			snprintf(r->label, sizeof(r->label), "%.*s", (int)len - 4, e->d_name);
			snprintf(r->sub, sizeof(r->sub), "%d songs", have);
			native_row tmp;
			download_row(dir, first, &tmp);
			snprintf(r->art, sizeof(r->art), "%s", tmp.art);
			r->is_playlist = true;   /* opens instead of playing */
		}
		closedir(ld);
	}

	DIR *d = opendir(dir);
	if (!d) {
		free(in_list);
		return n;   /* nothing saved yet: an empty list, not an error */
	}
	struct dirent *e;
	int hidden = 0;
	while (n < max && (e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (len < 5 || strcasecmp(e->d_name + len - 4, ".ogg") != 0)
			continue;
		const uint64_t h = name_hash(e->d_name);
		bool listed = false;
		for (int i = 0; i < in_n && !listed; i++)
			listed = in_list[i] == h;
		if (listed) {
			hidden++;
			continue;
		}
		download_row(dir, e->d_name, &rows[n++]);
	}
	closedir(d);
	free(in_list);
	nlog("FETCH downloads -> %d (%d inside saved lists)\n", n, hidden);
	return n;
}

/* The playlist list as last fetched, so a bad minute at Spotify does not
 * empty the home screen. "uri<TAB>name<TAB>art" per line beside the config. */
static void rootlist_cache_file(char *out, size_t n)
{
	char d[320];
	cfg_dir(d, sizeof(d));
	snprintf(out, n, "%splaylists.txt", d);
}

static void rootlist_cache_save(const native_row *rows, int n)
{
	char path[352], tmp[360];
	rootlist_cache_file(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++) {
		char label[64];
		snprintf(label, sizeof(label), "%s", rows[i].label);
		for (char *c = label; *c; c++)
			if (*c == '\t' || *c == '\n' || *c == '\r')
				*c = ' ';
		fprintf(f, "%s\t%s\t%s\n", rows[i].uri, label, rows[i].art);
	}
	if (fclose(f) == 0) {
		remove(path);
		rename(tmp, path);
	} else {
		remove(tmp);
	}
}

static int rootlist_cache_load(native_row *rows, int max)
{
	char path[352];
	rootlist_cache_file(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char line[512];
	int n = 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = '\0';
		char *a = strchr(line, '\t');
		char *b = a ? strchr(a + 1, '\t') : NULL;
		if (!b)
			continue;
		*a++ = '\0';
		*b++ = '\0';
		memset(&rows[n], 0, sizeof(rows[n]));
		snprintf(rows[n].uri, sizeof(rows[n].uri), "%s", line);
		snprintf(rows[n].label, sizeof(rows[n].label), "%s", a);
		snprintf(rows[n].art, sizeof(rows[n].art), "%s", b);
		rows[n].is_playlist = true;
		n++;
	}
	fclose(f);
	return n;
}

/* The search context only returns tracks, so the artists people expect on
 * top are read off the results: the best hit's artist, then any other that
 * comes up twice in the first twenty, three at most. They open the artist
 * view. No extra round trip: the batch metadata already named them. */
static int search_add_artists(native_row *rows, int n, int max)
{
	enum { SCAN = 20, MAX_ARTISTS = 3 };
	int pick[MAX_ARTISTS], k = 0;
	const int scan = n < SCAN ? n : SCAN;
	for (int i = 0; i < scan && k < MAX_ARTISTS; i++) {
		const char *a = rows[i].artist_uri;
		if (!a[0])
			continue;
		bool seen = false;
		for (int j = 0; j < k; j++)
			if (strcmp(rows[pick[j]].artist_uri, a) == 0)
				seen = true;
		if (seen)
			continue;
		int count = 0;
		for (int j = 0; j < scan; j++)
			if (strcmp(rows[j].artist_uri, a) == 0)
				count++;
		if (i == 0 || count >= 2)
			pick[k++] = i;
	}
	if (k == 0)
		return n;
	native_row art[MAX_ARTISTS];
	for (int j = 0; j < k; j++) {
		const native_row *src = &rows[pick[j]];
		memset(&art[j], 0, sizeof(art[j]));
		snprintf(art[j].uri, sizeof(art[j].uri), "%s", src->artist_uri);
		snprintf(art[j].label, sizeof(art[j].label), "%s", src->sub);
		snprintf(art[j].sub, sizeof(art[j].sub), "Artist");
		snprintf(art[j].art, sizeof(art[j].art), "%s", src->art);
		art[j].is_playlist = true;   /* opens instead of playing */
	}
	if (n + k > max)
		n = max - k;
	memmove(&rows[k], &rows[0], (size_t)n * sizeof(rows[0]));
	memcpy(&rows[0], art, (size_t)k * sizeof(rows[0]));
	return n + k;
}

static int fetch_view(native_view view, const char *arg, native_row *rows,
                      int max, char *err, size_t errlen);

/* A list: with a token renewed ahead of time, and once more after a 401
 * (a long sleep can outlast the margin). */
int native_fetch(native_view view, const char *arg, native_row *rows, int max,
                 char *err, size_t errlen)
{
	if (view != NATIVE_DOWNLOADS)
		l5_refresh(false);
	int n = fetch_view(view, arg, rows, max, err, errlen);
	if (n < 0 && view != NATIVE_DOWNLOADS && strstr(err, "HTTP 401")) {
		nlog("FETCH: 401, renewing the token\n");
		l5_refresh(true);
		n = fetch_view(view, arg, rows, max, err, errlen);
	}
	return n;
}

static int fetch_view(native_view view, const char *arg, native_row *rows,
                      int max, char *err, size_t errlen)
{
	if (max > NATIVE_ROWS)
		max = NATIVE_ROWS;
	memset(rows, 0, sizeof(*rows) * (size_t)max);

	/* The downloads shelf is the one view that owes nothing to the network:
	 * it answers from the SD even before sign-in, which is what makes the
	 * offline mode more than an error screen. */
	if (view == NATIVE_DOWNLOADS)
		return fetch_downloads(arg, rows, max);

	if (!g.ready) {
		snprintf(err, errlen, "not signed in");
		return -1;
	}
	if (g.offline) {
		snprintf(err, errlen, "offline");
		return -1;
	}

	static char uris[NATIVE_ROWS][64];
	const char *token = l5_token();
	fetch_cancel = false;   /* a cancel aimed at the previous fetch dies here */

	nlog("FETCH %s arg=%s max=%d\n", view_name(view), arg ? arg : "-", max);

	switch (view) {
	case NATIVE_PLAYLISTS: {
		static char names[NATIVE_ROWS][64];
		static char arts[NATIVE_ROWS][80];
		int count = 0;
		int n = sp_rootlist(g.sp_host, token, g.username, uris, names, arts,
		                    max, &count, err, errlen);
		if (n < 0 && strstr(err, "HTTP 5")) {
			/* Spotify's playlist service has bad minutes (502s); one
			 * more try after a beat often lands. */
			svcSleepThread(1500000000LL);
			n = sp_rootlist(g.sp_host, token, g.username, uris, names, arts,
			                max, &count, err, errlen);
		}
		if (n < 0) {
			/* Still failing: show the last list that worked rather than
			 * an empty screen with only Liked Songs and Downloads. */
			int c = rootlist_cache_load(rows, max);
			if (c > 0) {
				nlog("  rootlist failed (%s), %d playlists from the card\n",
				     err, c);
				return c;
			}
			return -1;
		}
		for (int i = 0; i < n; i++) {
			snprintf(rows[i].uri, sizeof(rows[i].uri), "%s", uris[i]);
			snprintf(rows[i].label, sizeof(rows[i].label), "%s",
			         names[i][0] ? names[i] : "Untitled playlist");
			snprintf(rows[i].art, sizeof(rows[i].art), "%s", arts[i]);
			rows[i].sub[0] = '\0';
			rows[i].is_playlist = true;
		}
		nlog("  -> %d playlists (total %d)\n", n, count);
		rootlist_cache_save(rows, n);
		return n;
	}
	case NATIVE_LIKED: {
		int n = sp_liked(g.sp_host, token, g.username, uris, max, err, errlen);
		if (n < 0)
			return -1;
		nlog("  liked: %d tracks%s\n", n, n >= max ? " (row cap)" : "");
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_SEARCH: {
		if (!arg || !arg[0]) {
			snprintf(err, errlen, "empty query");
			return -1;
		}
		int n = sp_search_tracks(g.sp_host, token, arg, uris, max, err, errlen);
		if (n < 0)
			return -1;
		n = name_tracks(uris, n, rows, max, err, errlen);
		if (n <= 0)
			return n;
		return search_add_artists(rows, n, max);
	}
	case NATIVE_TRACKS: {
		/* An album or an artist opened from the player: the context
		 * service lists either (an artist as its top tracks). */
		if (arg && (strncmp(arg, "spotify:album:", 14) == 0 ||
		            strncmp(arg, "spotify:artist:", 15) == 0)) {
			int n = sp_context_tracks(g.sp_host, token, arg, uris, max,
			                          err, errlen);
			if (n < 0)
				return -1;
			n = name_tracks(uris, n, rows, max, err, errlen);
			if (n < 0 || strncmp(arg, "spotify:artist:", 15) != 0)
				return n;
			/* An artist's context is only its top ten: the albums and
			 * singles follow as rows that open like a playlist. */
			static sp_release rel[NATIVE_ROWS];
			char rerr[160] = "";
			http_session *s = http_session_open(g.sp_host);
			int m = sp_artist_releases(s, g.sp_host, token, arg, rel,
			                           max - n, rerr, sizeof(rerr));
			http_session_close(s);
			if (m < 0)
				nlog("  releases failed: %s\n", rerr);
			for (int i = 0; i < m && n < max; i++, n++) {
				native_row *r = &rows[n];
				memset(r, 0, sizeof(*r));
				snprintf(r->uri, sizeof(r->uri), "%s", rel[i].uri);
				snprintf(r->label, sizeof(r->label), "%s",
				         rel[i].name[0] ? rel[i].name : "Album");
				if (rel[i].year)
					snprintf(r->sub, sizeof(r->sub), "%s · %d",
					         rel[i].type == 2 ? "Single" : "Album", rel[i].year);
				else
					snprintf(r->sub, sizeof(r->sub), "%s",
					         rel[i].type == 2 ? "Single" : "Album");
				snprintf(r->art, sizeof(r->art), "%s", rel[i].art);
				r->is_playlist = true;   /* opens instead of playing */
			}
			nlog("  artist: %d releases\n", m);
			return n;
		}
		if (!arg || strncmp(arg, "spotify:playlist:", 17) != 0) {
			snprintf(err, errlen, "not a playlist uri");
			return -1;
		}
		int count = 0;
		char name[128];
		int n = sp_playlist(g.sp_host, token, arg + 17, name, sizeof(name),
		                    uris, max, &count, err, errlen);
		if (n < 0)
			return -1;
		nlog("  playlist: %d of %d items%s\n", n, count,
		     n >= max ? " (row cap)" : "");
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_RADIO: {
		if (!arg || !arg[0]) {
			snprintf(err, errlen, "no seed track");
			return -1;
		}
		/* A radio is a mix, not a whole list: a hundred songs is plenty,
		 * and sp_track is too big to keep a full row cap of them. */
		static sp_track radio[100];
		int rmax = max < 100 ? max : 100;
		int n = sp_radio(g.sp_host, token, arg, rmax, radio, rmax, err, errlen);
		if (n < 0)
			return -1;
		/* radio-apollo carries the titles but no cover URLs, and the list
		 * rows draw their thumbnails from art: pay the one batch trip like
		 * every other track view, or the rows stay blank. */
		for (int i = 0; i < n; i++)
			snprintf(uris[i], sizeof(uris[i]), "%s", radio[i].uri);
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	}
	snprintf(err, errlen, "unknown view");
	return -1;
}

/* The playlist's real name, fetched when it is opened rather than for every
 * row of the rootlist. */
int native_playlist_name(const char *uri, char *name, size_t namelen)
{
	if (!g.ready || !uri || strncmp(uri, "spotify:playlist:", 17) != 0)
		return -1;
	char err[160];
	char throwaway[1][64];
	int count = 0;
	return sp_playlist(g.sp_host, l5_token(), uri + 17, name, namelen,
	                   throwaway, 1, &count, err, sizeof(err)) < 0 ? -1 : 0;
}
