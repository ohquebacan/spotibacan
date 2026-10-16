#include "native.h"

#include "ap3ds.h"
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
	}
	if (logf) {
		fputs(line, logf);
		fflush(logf);
	}
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
	case NATIVE_ALBUM:     return "album";
	case NATIVE_ARTIST:    return "artist";
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
	Thread        th;
	char          uri[64];   /* the requested track: meta.uri is not
	                          * filled by single-track metadata, so the
	                          * queue could not re-ask for it otherwise */
	volatile bool stop;
	volatile bool save;      /* write the .ogg (+ cover) when complete */
	volatile bool saved;     /* CAS guard: exactly one save per job */
	/* Ownership handoff between the player and the download thread:
	 * 0 both alive, 1 player detached (thread frees), 2 thread finished
	 * (player joins and frees). The CAS makes the handoff race-free. */
	volatile int  handoff;
	struct dl_job *next;
} dl_job;

/* The download queue: an armed download whose background slot is taken by a
 * newer one lands here instead of dying. Drained one at a time by the
 * player thread whenever no other download is using the link. */
typedef struct {
	char uri[64];
	char artist[128];
	char title[128];
} dlq_entry;

static dlq_entry dlq[NATIVE_DLQ_MAX];
static int dlq_count;                    /* guarded by g.lock */
static volatile int dlq_active_pct = -1; /* 0-100 while draining, else -1 */
static char dlq_active_title[128];

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

static struct {
	bool          ready;
	login5_token  l5;
	/* Serialises token refreshes: the player thread, the metadata fetch
	 * and the download queue all read g.l5, and a refresh rewrites it. */
	LightLock     l5_lock;
	char          sp_host[128];
	char          username[128];
	ap_session   *ap;
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
	/* Seek request in samples, -1 = none. Set by the UI, consumed by the
	 * decoder, which may reject it when the target is not downloaded yet. */
	volatile long  want_seek_frame;

	volatile native_state state;
	char          title[128];
	char          artist[128];
	char          album[128];
	/* "Go to album / artist" targets for the playing track, from the
	 * metadata gids. Empty for local files or missing gids. */
	char          now_album_uri[40];
	char          now_artist_uri[40];
	char          art[200];
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

	/* Offline mode: the player thread runs but never touches the network.
	 * Set once at init, read everywhere, never cleared. */
	bool          offline;
} g;

void native_set_downloads(bool on) { g.downloads_on = on; }

void native_save_current(void)
{
	g.want_save = !g.want_save;
	if (g.job) {
		g.job->save = g.want_save;
		/* The download may already be complete (a 3-minute song lands in
		 * seconds), in which case the download thread checked the flag
		 * long ago and nobody else will save it: do it here. The CAS
		 * keeps this from racing the thread's own save. */
		if (g.want_save && g.job->ts.dl_done && !g.job->ts.dl_failed &&
		    !__sync_lock_test_and_set(&g.job->saved, 1))
			save_download(&g.job->ts);
	}
}
bool native_save_armed(void) { return g.want_save; }

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
	int pct = dlq_active_pct;
	if (pct >= 0)
		snprintf(title_out, titlelen, "%s", dlq_active_title);
	return pct;
}

/* Appends a track to the download queue, unless it is already queued or
 * already on the card. Caller holds no lock. Returns false when the queue
 * is full: twelve pending downloads is plenty for a handheld. */
static bool dlq_enqueue(const char *uri, const char *artist,
                        const char *title)
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
	nlog("DLQ: en cola %s - %s\n", artist, title);
	return true;
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
		snprintf(err, errlen, "sin fuente de entropia (ps:ps)");
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
				snprintf(err, errlen, "cancelado");
				return -1;
			}

		spotify_tokens tok;
		char perr[192] = "";
		pair_status st = pair_poll(&req, &tok, perr, sizeof(perr));
		if (st == PAIR_OK) {
			if (!tok.refresh_token[0] ||
			    !cfg_save_refresh_token(tok.refresh_token, tok.scope)) {
				snprintf(err, errlen, "no se pudo guardar el token");
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
			snprintf(err, errlen, "%s", perr[0] ? perr : "el pareo fallo");
			return -1;
		}
	}
	snprintf(err, errlen, "el codigo caduco");
	return -1;
}

native_state native_playback_state(void) { return g.state; }
const char *native_now_title(void)  { return g.title; }
const char *native_now_artist(void) { return g.artist; }
const char *native_now_album(void)  { return g.album; }
/* The URI last asked for, so the lyrics view can tell its document belongs
 * to what is playing. Empty before the first play. */
const char *native_now_uri(void)    { return g.want; }
/* "Go to" targets; empty for local files or when the metadata had no gids. */
const char *native_now_album_uri(void)  { return g.now_album_uri; }
const char *native_now_artist_uri(void) { return g.now_artist_uri; }
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
		snprintf(err, errlen, "uri o token invalido");
		return -1;
	}
	return sp_library_modify(g.access_token, id + 1, like, err, errlen,
	                         retry_after);
}

/* Builds the music/ path a track saves to, so the writer and the "already
 * saved?" check can never disagree on the name. */
static void download_path(const char *artist, const char *title,
                          char *out, size_t outlen)
{
	char dir[352];
	cfg_dir(dir, sizeof(dir));
	strncat(dir, "music", sizeof(dir) - strlen(dir) - 1);
	mkdir(dir, 0777);

	char name[160];
	int n = 0;
	for (const char *p = artist; *p && n < 60; p++)
		if (*p != '/' && *p != '\\' && *p != ':' && *p != '"' &&
		    *p != '?' && *p != '*' && *p != '<' && *p != '>' && *p != '|')
			name[n++] = *p;
	n += snprintf(name + n, sizeof(name) - n, " - ");
	for (const char *p = title; *p && n < 130; p++)
		if (*p != '/' && *p != '\\' && *p != ':' && *p != '"' &&
		    *p != '?' && *p != '*' && *p != '<' && *p != '>' && *p != '|')
			name[n++] = *p;
	snprintf(name + n, sizeof(name) - n, ".ogg");

	snprintf(out, outlen, "%s/%s", dir, name);
}

/* Whether the playing track already sits on the card. One stat per track:
 * the answer is cached against the URI, because the UI asks every frame. */
static char saved_checked_uri[192];
static bool saved_flag;

bool native_current_saved(void)
{
	if (strncmp(g.want, "file:", 5) == 0)
		return true;   /* a local file is on the card by definition */
	if (!g.title[0])
		return false;
	if (strcmp(saved_checked_uri, g.want) != 0) {
		snprintf(saved_checked_uri, sizeof(saved_checked_uri), "%s", g.want);
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

	char tmp[528];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		nlog("DL: no se pudo guardar %s\n", path);
		return;
	}
	fwrite(ts->data + SPOTIFY_OGG_HEADER, 1, ts->len - SPOTIFY_OGG_HEADER, f);
	fclose(f);
	rename(tmp, path);
	/* The saved check caches per URI; make it look again. */
	saved_checked_uri[0] = '\0';
	nlog("DL: guardada %s (%.1f MB)\n", path,
	     (double)(ts->len - SPOTIFY_OGG_HEADER) / (1024.0 * 1024.0));

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
	track_stream_download(&job->ts, &job->stop);
	/* Saving on the way out would hold HOME hostage behind an SD write
	 * plus a cover fetch: quitting drops the save, the queue entry was
	 * already consumed anyway. */
	if (job->save && !g.quit && job->ts.dl_done && !job->ts.dl_failed &&
	    !__sync_lock_test_and_set(&job->saved, 1))
		save_download(&job->ts);
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
	track_stream_free(&job->ts);
	free(job);
}

/* Leaves a job with its download thread (the player is moving on). When the
 * thread already finished, the job is reclaimed here instead. */
static void job_detach(dl_job *job)
{
	__sync_synchronize();
	if (__sync_bool_compare_and_swap(&job->handoff, 0, 1))
		return;   /* the thread will free itself when it finishes */
	threadJoin(job->th, U64_MAX);
	threadFree(job->th);
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
			         ts->dl_err[0] ? ts->dl_err : "la descarga fallo");
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
		         "el decodificador rechazo el stream (%d)", verr);
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
		         ts->dl_err[0] ? ts->dl_err : "la descarga fallo a mitad");
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

	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(g.error, sizeof(g.error), "no se encontro el archivo");
		g.state = NATIVE_FAILED;
		return;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *data = malloc((size_t)len);
	if (!data || fread(data, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(data);
		snprintf(g.error, sizeof(g.error), "no se pudo leer el archivo");
		g.state = NATIVE_FAILED;
		return;
	}
	fclose(f);

	int verr = 0;
	stb_vorbis *v = stb_vorbis_open_memory(data, (int)len, &verr, NULL);
	if (!v) {
		free(data);
		snprintf(g.error, sizeof(g.error),
		         "el decodificador rechazo el archivo (%d)", verr);
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
	free(data);
	nlog("PLAY local fin: %u frames\n", g.frames_out);
}

/* The login5 token carries a ~1 h expiry, after which spclient answers
 * 401 and every list fetch fails until the app restarts. The login5
 * response hands back fresh reusable credentials, so an expired token can
 * be traded for a new one without re-entering through the access point.
 * Callers get a copy of the current token; the refresh itself happens
 * under l5_lock, at most one thread at a time. A failed refresh backs off
 * for a minute so a network outage does not hammer login5 on every call. */
static void l5_token(char *out, size_t outlen)
{
	LightLock_Lock(&g.l5_lock);
	time_t now = time(NULL);
	if (now >= g.l5.expires_at) {
		if (g.l5.stored_len > 0) {
			login5_token fresh;
			char err[192] = "";
			if (login5_from_credentials(g.username, g.l5.stored,
			                            g.l5.stored_len, g.device_id,
			                            &fresh, err, sizeof(err)) == 0) {
				nlog("L5: token renovado (quedan %ld s)\n",
				     (long)(fresh.expires_at - now));
				g.l5 = fresh;
			} else {
				nlog("L5: renovacion fallo (%s); reintento en 60 s\n",
				     err);
				g.l5.expires_at = time(NULL) + 60;
			}
		} else {
			nlog("L5: sin credenciales reutilizables; no se puede renovar\n");
			g.l5.expires_at = time(NULL) + 60;
		}
	}
	snprintf(out, outlen, "%s", g.l5.access_token);
	LightLock_Unlock(&g.l5_lock);
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
				play_local(uri + 5);
				if (g.state == NATIVE_PLAYING)
					g.state = NATIVE_IDLE;
				continue;
			}

			nlog("FETCH track %s\n", uri);

			/* The idle loop drops g.ap when the access point hangs up; the
			 * background reconnect below rebuilds it, so a track asked for
			 * right after a hiccup only has to wait for it. */
			if (!g.ap && !ap_reconnect()) {
				snprintf(g.error, sizeof(g.error),
				         "reconectando con Spotify, intenta de nuevo");
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
				snprintf(g.error, sizeof(g.error), "sin memoria");
				g.state = NATIVE_FAILED;
				continue;
			}
			snprintf(job->uri, sizeof(job->uri), "%s", uri);
			char err[192];
			char token[sizeof(g.l5.access_token)];
			l5_token(token, sizeof(token));
			int rc = track_stream_begin(g.sp_host, token, g.ap,
			                            uri, NULL, &job->ts, err, sizeof(err));
			if (rc != 0 &&
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
				if (ap_reconnect())
					rc = track_stream_begin(g.sp_host, token,
					                        g.ap, uri, NULL, &job->ts, err,
					                        sizeof(err));
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
			snprintf(g.now_album_uri, sizeof(g.now_album_uri), "%s",
			         job->ts.meta.album_uri);
			snprintf(g.now_artist_uri, sizeof(g.now_artist_uri), "%s",
			         job->ts.meta.artist_uri);
			snprintf(g.art, sizeof(g.art), "%s", job->ts.meta.art_large);

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
			/* The fetch runs a full TLS handshake (cert chain +
			 * RSA bignum) on this stack; 48 KB was not always enough. */
			Thread dl = threadCreate(download_thread, job, 96 * 1024,
			                         0x31, -2, false);
			if (!dl) {
				snprintf(g.error, sizeof(g.error),
				         "no se pudo crear el hilo de descarga");
				g.state = NATIVE_FAILED;
				LightLock_Lock(&g.lock);
				g.jobs = job->next;
				g.job = NULL;
				LightLock_Unlock(&g.lock);
				track_stream_free(&job->ts);
				free(job);
				continue;
			}
			job->th = dl;
			play_stream(&job->ts);
			if (g.quit) {
				/* Shutting down: nothing outlives the player. */
				job->stop = true;
				threadJoin(dl, U64_MAX);
				threadFree(dl);
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
					if (p != job && p->handoff == 1 &&
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

		/* The download queue drains whenever the link is idle: nothing
		 * playing, or the playing track's file already complete. One at a
		 * time, so the queue never doubles the big stream buffers. */
		if (dlq_count > 0 && g.ready) {
			bool busy = false;
			LightLock_Lock(&g.lock);
			for (dl_job *p = g.jobs; p; p = p->next)
				if (!p->ts.dl_done && !p->ts.dl_failed)
					busy = true;
			LightLock_Unlock(&g.lock);
			if (!busy) {
				dlq_entry e;
				LightLock_Lock(&g.lock);
				e = dlq[0];
				memmove(&dlq[0], &dlq[1],
				        (size_t)(dlq_count - 1) * sizeof(dlq[0]));
				dlq_count--;
				LightLock_Unlock(&g.lock);

				/* Saved another way while it waited? Nothing to do. */
				char path[520];
				download_path(e.artist, e.title, path, sizeof(path));
				struct stat st;
				if (stat(path, &st) == 0 && st.st_size > 0) {
					nlog("DLQ: %s ya estaba en la tarjeta\n", e.title);
					continue;
				}

				snprintf(dlq_active_title, sizeof(dlq_active_title),
				         "%s - %s", e.artist, e.title);
				dlq_active_pct = 0;

				dl_job *job = calloc(1, sizeof(*job));
				if (job)
					snprintf(job->uri, sizeof(job->uri), "%s", e.uri);
				char err[192];
				char token[sizeof(g.l5.access_token)];
				l5_token(token, sizeof(token));
				int rc = job ? track_stream_begin(g.sp_host,
				                   token, g.ap, e.uri,
				                   NULL, &job->ts, err, sizeof(err)) : -1;
				if (rc != 0) {
					nlog("DLQ: no se pudo empezar %s (%s)\n", e.title,
					     job ? err : "sin memoria");
					free(job);
					dlq_active_pct = -1;
					continue;   /* one bad entry loses its turn only */
				}
				job->save = true;
				LightLock_Lock(&g.lock);
				job->next = g.jobs;
				g.jobs = job;
				LightLock_Unlock(&g.lock);
				Thread dl = threadCreate(download_thread, job,
				                         96 * 1024, 0x31, -2, false);
				if (!dl) {
					nlog("DLQ: sin hilo para %s\n", e.title);
					LightLock_Lock(&g.lock);
					g.jobs = job->next;
					LightLock_Unlock(&g.lock);
					track_stream_free(&job->ts);
					free(job);
					dlq_active_pct = -1;
					continue;
				}
				job->th = dl;
				nlog("DLQ: descargando %s\n", e.title);
				unsigned long long ka = osGetTime();
				while (!g.quit && !g.want_new &&
				       !job->ts.dl_done && !job->ts.dl_failed) {
					if (job->ts.len)
						dlq_active_pct =
						    (int)(job->ts.downloaded * 100 /
						          job->ts.len);
					/* The session still needs its pings answered while
					 * the queue works, or the next entry's key fails. */
					unsigned long long now = osGetTime();
					if (g.ap && now - ka >= 1000) {
						ka = now;
						if (ap_wait(g.ap, 0) == 1) {
							static uint8_t ka_buf[2048];
							uint8_t cmd;
							int kn = ap_recv(g.ap, &cmd, ka_buf,
							                 sizeof(ka_buf));
							if (kn < 0) {
								ap_close(g.ap);
								g.ap = NULL;
							} else if (cmd == AP_PKT_PING) {
								ap_handle_ping(g.ap, ka_buf,
								               (size_t)kn);
							}
						}
					}
					svcSleepThread(200000000LL);   // 200 ms
				}
				dlq_active_pct = -1;
				if (g.quit) {
					job->stop = true;
					threadJoin(dl, U64_MAX);
					threadFree(dl);
					LightLock_Lock(&g.lock);
					dl_job **p = &g.jobs;
					while (*p && *p != job)
						p = &(*p)->next;
					if (*p)
						*p = job->next;
					LightLock_Unlock(&g.lock);
					track_stream_free(&job->ts);
					free(job);
				} else if (g.want_new) {
					/* The user picked a track: that stream owns the link
					 * now; this one keeps the background slot and saves
					 * itself when it lands. */
					job_detach(job);
				} else {
					/* Finished (or failed) with the player idle. */
					threadJoin(dl, U64_MAX);
					threadFree(dl);
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
				continue;   /* another entry, or a play request */
			}
		}

		/* Idle: keep the access point session alive. It only speaks to say
		 * ping, but a session that stops answering is dropped, and then the
		 * next audio key would fail for a reason that looks like anything
		 * but a missed ping. */
		if (ap_wait(g.ap, 200) == 1) {
			uint8_t buf[2048];
			uint8_t cmd;
			int n = ap_recv(g.ap, &cmd, buf, sizeof(buf));
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
		snprintf(err, errlen, "sin destino de audio");
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

	step(progress, "Reading the stored token...");
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		snprintf(err, errlen, "sin parear");
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
	LightLock_Init(&g.l5_lock);

	char ignored[128];
	sp_resolve(g.sp_host, sizeof(g.sp_host), ignored, sizeof(ignored));

	LightLock_Init(&g.lock);
	g.want_seek_frame = -1;
	g.state = NATIVE_IDLE;
	g.ready = true;
	/* track_stream_begin() handshakes TLS on this thread; 64 KB overflowed
	 * on unlucky cert chains (two crash dumps died mid-handshake). */
	g.player = threadCreate(player_thread, NULL, 128 * 1024, 0x30, -2, false);
	if (!g.player) {
		snprintf(err, errlen, "no se pudo crear el hilo del reproductor");
		g.ready = false;
		return -1;
	}
	nlog("INIT: ok, %s via %s\n", g.username, g.sp_host);
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
	snprintf(g.username, sizeof(g.username), "Sin conexion");
	LightLock_Init(&g.lock);
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

void native_exit(void)
{
	if (!g.ready)
		return;
	g.quit = true;
	if (g.player) {
		threadJoin(g.player, 2000000000LL);
		threadFree(g.player);
	}
	if (g.ap)
		ap_close(g.ap);
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
	char token[sizeof(g.l5.access_token)];
	l5_token(token, sizeof(token));
	int done = 0;
	for (int base = 0; base < n; base += 50) {
		if (fetch_cancel) {
			snprintf(err, errlen, "cancelado");
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
		    ? sp_track_metadata_batch_s(sess, token, ptrs,
		                                chunk, meta, err, errlen)
		    : sp_track_metadata_batch(g.sp_host, token, ptrs,
		                              chunk, meta, err, errlen);
		osTickCounterUpdate(&t);
		nlog("  chunk %d..%d -> %d named%s%s (%.0f ms%s)\n", base,
		     base + chunk - 1, named, named < 0 ? ": " : "",
		     named < 0 ? err : "", osTickCounterRead(&t),
		     sess && http_session_last_reused(sess) ? ", ka" : "");
		if (named < 0)
			continue;
		for (int i = 0; i < named; i++) {
			native_row *r = &rows[base + i];
			snprintf(r->label, sizeof(r->label), "%s",
			         meta[i].name[0] ? meta[i].name : "(sin nombre)");
			snprintf(r->sub, sizeof(r->sub), "%s", meta[i].artist);
			snprintf(r->uri, sizeof(r->uri), "%s",
			         meta[i].uri[0] ? meta[i].uri : uris[base + i]);
			snprintf(r->art, sizeof(r->art), "%s", meta[i].art_small);
			snprintf(r->artist_uri, sizeof(r->artist_uri), "%s",
			         meta[i].artist_uri);
			snprintf(r->album_uri, sizeof(r->album_uri), "%s",
			         meta[i].album_uri);
			r->is_playlist = false;
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
		snprintf(rows[i].label, sizeof(rows[i].label), "(sin nombre)");
		rows[i].is_playlist = false;
	}
	return n;
}

/* Lists the .ogg files under music/ as playable rows, newest first (the
 * order the saver produced them in is the order the user last heard).
 * Pure filesystem: no session, no network. */
static int fetch_downloads(native_row *rows, int max)
{
	char dir[352];
	cfg_dir(dir, sizeof(dir));
	strncat(dir, "music", sizeof(dir) - strlen(dir) - 1);

	DIR *d = opendir(dir);
	if (!d)
		return 0;   /* nothing saved yet: an empty list, not an error */

	int n = 0;
	struct dirent *e;
	while (n < max && (e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (len < 5 || strcasecmp(e->d_name + len - 4, ".ogg") != 0)
			continue;
		native_row *r = &rows[n++];
		snprintf(r->uri, sizeof(r->uri), "file:%s", e->d_name);
		snprintf(r->label, sizeof(r->label), "%.*s", (int)len - 4,
		         e->d_name);
		struct stat st;
		char path[400];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st) == 0)
			snprintf(r->sub, sizeof(r->sub), "%.1f MB",
			         (double)st.st_size / (1024.0 * 1024.0));
		/* A saved <name>.jpg beside the .ogg is the row's thumbnail; the
		 * file: scheme tells artwork.c to read it off the SD. */
		char jpg[200];
		snprintf(jpg, sizeof(jpg), "%.*s.jpg", (int)len - 4, e->d_name);
		snprintf(path, sizeof(path), "%s/%s", dir, jpg);
		if (stat(path, &st) == 0 && st.st_size > 0)
			snprintf(r->art, sizeof(r->art), "file:%s", jpg);
	}
	closedir(d);
	nlog("FETCH downloads -> %d\n", n);
	return n;
}

int native_fetch(native_view view, const char *arg, native_row *rows, int max,
                 char *err, size_t errlen)
{
	if (max > NATIVE_ROWS)
		max = NATIVE_ROWS;
	memset(rows, 0, sizeof(*rows) * (size_t)max);

	/* The downloads shelf is the one view that owes nothing to the network:
	 * it answers from the SD even before sign-in, which is what makes the
	 * offline mode more than an error screen. */
	if (view == NATIVE_DOWNLOADS)
		return fetch_downloads(rows, max);

	if (!g.ready) {
		snprintf(err, errlen, "not signed in");
		return -1;
	}
	if (g.offline) {
		snprintf(err, errlen, "sin conexion");
		return -1;
	}

	static char uris[NATIVE_ROWS][64];
	char token[sizeof(g.l5.access_token)];
	l5_token(token, sizeof(token));
	fetch_cancel = false;   /* a cancel aimed at the previous fetch dies here */

	nlog("FETCH %s arg=%s max=%d\n", view_name(view), arg ? arg : "-", max);

	switch (view) {
	case NATIVE_PLAYLISTS: {
		static char names[NATIVE_ROWS][64];
		static char arts[NATIVE_ROWS][80];
		int count = 0;
		int n = sp_rootlist(g.sp_host, token, g.username, uris, names, arts,
		                    max, &count, err, errlen);
		if (n < 0)
			return -1;
		for (int i = 0; i < n; i++) {
			snprintf(rows[i].uri, sizeof(rows[i].uri), "%s", uris[i]);
			snprintf(rows[i].label, sizeof(rows[i].label), "%s",
			         names[i][0] ? names[i] : "Lista sin nombre");
			snprintf(rows[i].art, sizeof(rows[i].art), "%s", arts[i]);
			rows[i].sub[0] = '\0';
			rows[i].is_playlist = true;
		}
		nlog("  -> %d playlists (total %d)\n", n, count);
		return n;
	}
	case NATIVE_LIKED: {
		int n = sp_liked(g.sp_host, token, g.username, uris, max, err, errlen);
		if (n < 0)
			return -1;
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
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_TRACKS: {
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
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_RADIO: {
		if (!arg || !arg[0]) {
			snprintf(err, errlen, "no seed track");
			return -1;
		}
		static sp_track radio[NATIVE_ROWS];
		int n = sp_radio(g.sp_host, token, arg, max, radio, max, err, errlen);
		if (n < 0)
			return -1;
		/* radio-apollo carries the titles but no cover URLs, and the list
		 * rows draw their thumbnails from art: pay the one batch trip like
		 * every other track view, or the rows stay blank. */
		for (int i = 0; i < n; i++)
			snprintf(uris[i], sizeof(uris[i]), "%s", radio[i].uri);
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_ALBUM:
	case NATIVE_ARTIST: {
		/* arg is the album/artist URI; the metadata/4 answer carries named
		 * Track entries inline, so unlike playlists the rows fill directly
		 * with no naming round trip. */
		const char *prefix = view == NATIVE_ALBUM ? "spotify:album:"
		                                          : "spotify:artist:";
		const size_t plen = strlen(prefix);
		if (!arg || strncmp(arg, prefix, plen) != 0) {
			snprintf(err, errlen, "no %s in view argument",
			         view == NATIVE_ALBUM ? "album" : "artist");
			return -1;
		}
		static sp_brief briefs[128];
		char name[128];
		int count = 0;
		int n = (view == NATIVE_ALBUM ? sp_album_tracks : sp_artist_top)
		    (g.sp_host, token, arg + plen, briefs,
		     (int)(sizeof(briefs) / sizeof(briefs[0])), name, sizeof(name),
		     &count, err, errlen);
		if (n < 0)
			return -1;
		for (int i = 0; i < n; i++) {
			snprintf(rows[i].label, sizeof(rows[i].label), "%s",
			         briefs[i].name[0] ? briefs[i].name : "(sin nombre)");
			snprintf(rows[i].sub, sizeof(rows[i].sub), "%s", briefs[i].artist);
			snprintf(rows[i].uri, sizeof(rows[i].uri), "%s", briefs[i].uri);
			snprintf(rows[i].art, sizeof(rows[i].art), "%s",
			         briefs[i].art_small);
			snprintf(rows[i].artist_uri, sizeof(rows[i].artist_uri), "%s",
			         briefs[i].artist_uri);
			snprintf(rows[i].album_uri, sizeof(rows[i].album_uri), "%s",
			         briefs[i].album_uri);
			rows[i].is_playlist = false;
		}
		return n;
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
	char token[sizeof(g.l5.access_token)];
	l5_token(token, sizeof(token));
	return sp_playlist(g.sp_host, token, uri + 17, name, namelen,
	                   throwaway, 1, &count, err, sizeof(err)) < 0 ? -1 : 0;
}
