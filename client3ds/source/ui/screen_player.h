#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "touch.h"

/* Bottom screen, player view.
 *
 * Ported from the reference client's layout and stripped to what the native
 * backend can honestly drive today: three transport controls on one baseline
 * with no button boxes (previous / play-pause / next), a progress scrubber,
 * and a fixed header with the track title. The reference's recently-played
 * shelf, shuffle and repeat return when the backend grows a queue.
 *
 * The scrubber is a real seek: the backend streams the whole file into
 * memory, and a drag anywhere inside the downloaded span lands. A jump past
 * the download watermark is ignored by the decoder.
 */

/* Player touch ids live at 40+ on purpose: the list screen's ids start at 10
 * (SL_BTN_SEARCH), and both screens are registered on the same touch builder,
 * so a tap handled here must not double-fire as a list-screen tap in the same
 * frame (the shell's dispatch is a flat "first rect that contains the point").
 * BTN_P_RECENT0 stays at 0x10, which collides with nothing (0x10 == 16 > 13). */
enum {
	BTN_P_PLAY = 40,
	BTN_P_PREV,
	BTN_P_NEXT,
	BTN_P_LIBRARY,
	BTN_P_MUTE,
	BTN_P_LYRICS,
	BTN_P_SCRUB,
	BTN_P_SHUFFLE,
	BTN_P_REPEAT,
	BTN_P_LIKE,
	BTN_P_DOWNLOAD,
	BTN_P_QUEUE,
	BTN_P_ARTIST,
	BTN_P_ALBUM,
	BTN_P_RECENT0 = 0x10,   /* + index: replay a shelf item */
};

typedef struct {
	C2D_TextBuf buf;
	touch_builder *tb;

	const char *track;    /* header */
	const char *artist;   /* centre block */
	const char *album;

	bool playing;         /* the disc shows the pause glyph */
	bool loading;         /* centre block shows the animated badge instead */
	bool muted;
	bool shuffle;         /* next track is a random row */
	int  repeat;          /* 0 off, 1 all, 2 one */
	bool liked;           /* the heart, for the playing track */
	bool dl_show;         /* a remote track: the download button exists */
	bool dl_armed;        /* it will save when the background download ends */
	bool dl_saved;        /* already on the card: the icon is a check */
	int  dl_queue;        /* tracks waiting in the download queue */
	bool dl_active;       /* the queue is downloading something now */
	bool can_prev, can_next; /* no neighbour in the current view: greyed */

	long progress_ms;
	long duration_ms;

	/* Which control the finger is currently down on, or -1. Used only to
	 * give a pressed appearance. */
	int  pressed_id;

	unsigned animation_ms; /* drives the loading badge */

	/* Recently-played shelf: art URLs of the last tracks this console
	 * played, tapped to replay. recent_n <= 4 are drawn. */
	const char *const *recent_art;
	int recent_n;
	const C2D_Image *(*thumb)(const char *url, void *ctx);
	void *thumb_ctx;

	u32 accent;      /* the app's green: pressed glyphs and active states */
	u32 on_accent;
	u32 panel;       /* header background */
} screen_player_args;

void screen_player_draw(const screen_player_args *a);

/* Scrubber geometry, shared with the input handler for the day seek lands. */
#define SCRUB_BAR_X 50.0f
#define SCRUB_BAR_W 220.0f
#define SCRUB_BAR_Y 208.0f
