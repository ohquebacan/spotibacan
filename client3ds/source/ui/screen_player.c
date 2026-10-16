#include "screen_player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "lang.h"

/* Bottom screen is 320x240. The layout follows the reference client's player
 * stripped to three transport controls: prev / play-pause / next on one
 * baseline, a fixed header, and a display-only scrubber. */

#define HEADER_H   30.0f
#define ROW_Y      152.0f /* transport baseline */
#define CTRL_GAP   26.0f
#define CTRL_SMALL 34.0f
#define PLAY_R     20.0f /* 40px disc */

/* The shelf owns the middle: the track's name and artist already live on the
 * top screen, so repeating them here was space the covers could use. */
#define SHELF_LABEL_Y 44.0f
#define SHELF_Y      58.0f
#define SHELF_TILE   52.0f
#define SHELF_GAP    10.0f
#define SHELF_MAX    4

#define SCRUB_TIMES_Y 196.0f

#define CLR_WHITE   C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_DISC_FG C2D_Color32(0x0B, 0x0B, 0x0B, 0xFF)
#define CLR_IDLE    C2D_Color32(0xB3, 0xB3, 0xB3, 0xFF)
#define CLR_LABEL   C2D_Color32(0x6A, 0x6A, 0x6A, 0xFF)
#define CLR_TRACK   C2D_Color32(0x4D, 0x4D, 0x4D, 0xFF)
#define CLR_TILE    C2D_Color32(0x19, 0x19, 0x19, 0xFF)
#define CLR_TIME    C2D_Color32(0xB3, 0xB3, 0xB3, 0xFF)

/* citro2d packs RGBA with alpha in the top byte, so the pressed halo and the
 * disc's fade ring are the accent with its alpha swapped out. */
static u32 with_alpha(u32 clr, u8 alpha)
{
	return (clr & 0x00FFFFFFu) | ((u32)alpha << 24);
}

/* With the boxes gone there is no fill to highlight, so the glyph itself has
 * to carry the press: a pressed control turns accent-green and gets a soft
 * halo behind it, which is what actually makes the tap feel like it landed. */
static u32 press_clr(u32 clr, bool pressed, u32 accent)
{
	return pressed ? accent : clr;
}

static void press_halo(float cx, float cy, bool pressed, u32 accent)
{
	if (pressed)
		ui_disc(cx, cy, 19.0f, with_alpha(accent, 0x33));
}

static void tri_right(float x, float y, float w, float h, u32 clr)
{
	C2D_DrawTriangle(x, y, clr, x, y + h, clr, x + w, y + h / 2, clr, 0.0f);
}

static void tri_left(float x, float y, float w, float h, u32 clr)
{
	C2D_DrawTriangle(x + w, y, clr, x + w, y + h, clr, x, y + h / 2, clr,
	                 0.0f);
}

static void draw_prev(float cx, float cy, u32 clr)
{
	tri_left(cx - 6.0f, cy - 8.0f, 12.0f, 16.0f, clr);
	C2D_DrawRectSolid(cx - 9.0f, cy - 8.0f, 0.0f, 3.0f, 16.0f, clr);
}

static void draw_next(float cx, float cy, u32 clr)
{
	tri_right(cx - 6.0f, cy - 8.0f, 12.0f, 16.0f, clr);
	C2D_DrawRectSolid(cx + 6.0f, cy - 8.0f, 0.0f, 3.0f, 16.0f, clr);
}

static void draw_play_disc(float cx, float cy, bool pressed, u32 accent)
{
	const int segments = 32;
	const float inner_r = PLAY_R - 0.75f;
	const float outer_r = PLAY_R + 0.25f;
	const u32 fill = pressed ? accent : CLR_WHITE;
	const u32 clear = with_alpha(fill, 0x00);
	float inner_px = cx + inner_r;
	float inner_py = cy;
	float outer_px = cx + outer_r;
	float outer_py = cy;

	for (int i = 1; i <= segments; i++) {
		const float angle =
		    (float)i * 2.0f * (float)M_PI / (float)segments;
		const float inner_nx = cx + inner_r * cosf(angle);
		const float inner_ny = cy + inner_r * sinf(angle);
		const float outer_nx = cx + outer_r * cosf(angle);
		const float outer_ny = cy + outer_r * sinf(angle);

		C2D_DrawTriangle(cx, cy, fill, inner_px, inner_py, fill, inner_nx,
		                 inner_ny, fill, 0.0f);
		C2D_DrawTriangle(inner_px, inner_py, fill, outer_px, outer_py,
		                 clear, outer_nx, outer_ny, clear, 0.0f);
		C2D_DrawTriangle(inner_px, inner_py, fill, outer_nx, outer_ny,
		                 clear, inner_nx, inner_ny, fill, 0.0f);

		inner_px = inner_nx;
		inner_py = inner_ny;
		outer_px = outer_nx;
		outer_py = outer_ny;
	}
}

static void draw_playpause(float cx, float cy, bool playing, bool pressed,
                           u32 accent)
{
	/* The disc is already the brightest thing on screen, so it presses by
	 * tinting rather than by gaining a halo it would hide anyway. */
	draw_play_disc(cx, cy, pressed, accent);

	if (playing) {
		C2D_DrawRectSolid(cx - 6.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f,
		                  CLR_DISC_FG);
		C2D_DrawRectSolid(cx + 2.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f,
		                  CLR_DISC_FG);
	} else {
		/* Nudged right so the triangle looks centred in the disc. */
		tri_right(cx - 4.0f, cy - 8.0f, 14.0f, 16.0f, CLR_DISC_FG);
	}
}

/* Speaker glyph for the mute toggle: a box plus a two-triangle cone, the
 * same proven shape as the volume overlay. Audible gets two chevrons opening
 * to the right, muted gets an X. */
static void draw_speaker(float cx, float cy, bool muted, u32 clr)
{
	C2D_DrawRectSolid(cx - 11.0f, cy - 4.0f, 0.0f, 5.0f, 8.0f, clr);
	C2D_DrawTriangle(cx - 6.0f, cy - 4.0f, clr, cx, cy - 9.0f, clr,
	                 cx, cy + 9.0f, clr, 0.0f);
	C2D_DrawTriangle(cx - 6.0f, cy - 4.0f, clr, cx, cy + 9.0f, clr,
	                 cx - 6.0f, cy + 4.0f, clr, 0.0f);
	if (muted) {
		const float down[4] = {cx + 4.0f, cy - 5.0f, cx + 11.0f, cy + 5.0f};
		const float up[4]   = {cx + 11.0f, cy - 5.0f, cx + 4.0f, cy + 5.0f};
		ui_polyline(down, 2, 2.0f, clr);
		ui_polyline(up, 2, 2.0f, clr);
	} else {
		const float near[6] = {cx + 3.0f, cy - 4.0f, cx + 6.5f, cy,
		                       cx + 3.0f, cy + 4.0f};
		const float far[6]  = {cx + 7.0f, cy - 7.0f, cx + 11.5f, cy,
		                       cx + 7.0f, cy + 7.0f};
		ui_polyline(near, 3, 1.8f, clr);
		ui_polyline(far, 3, 1.8f, clr);
	}
}

/* Shuffle: two crossing paths with arrowheads. Repeat: a loop with an
 * arrowhead; repeat-one adds a small 1. Heart: two discs over a wedge. */
static void draw_shuffle(float cx, float cy, u32 clr)
{
	const float hi[8] = {cx - 9, cy - 6, cx - 4, cy - 6, cx + 4, cy + 6,
	                     cx + 7, cy + 6};
	const float lo[8] = {cx - 9, cy + 6, cx - 4, cy + 6, cx + 4, cy - 6,
	                     cx + 7, cy - 6};
	ui_polyline(hi, 4, 1.8f, clr);
	ui_polyline(lo, 4, 1.8f, clr);
	tri_right(cx + 6.0f, cy - 9.0f, 6.0f, 6.0f, clr);
	tri_right(cx + 6.0f, cy + 3.0f, 6.0f, 6.0f, clr);
}

static void draw_repeat(float cx, float cy, bool one, u32 clr)
{
	const float pts[14] = {
		cx + 2, cy - 8,  cx - 4, cy - 8,  cx - 8, cy - 4,
		cx - 8, cy + 4,  cx - 4, cy + 8,  cx + 4, cy + 8,
		cx + 8, cy + 4,
	};
	ui_polyline(pts, 7, 1.8f, clr);
	tri_right(cx + 4.0f, cy - 11.0f, 6.0f, 6.0f, clr);
	if (one)
		ui_disc(cx + 7.0f, cy - 7.0f, 3.0f, clr);
}

static void draw_heart(float cx, float cy, bool liked, u32 clr_on, u32 clr_off)
{
	const u32 clr = liked ? clr_on : clr_off;
	ui_disc(cx - 3.5f, cy - 2.5f, 4.0f, clr);
	ui_disc(cx + 3.5f, cy - 2.5f, 4.0f, clr);
	C2D_DrawTriangle(cx - 7.0f, cy - 1.5f, clr, cx + 7.0f, cy - 1.5f, clr,
	                 cx, cy + 7.5f, clr, 0.0f);
}

/* Download: an arrow into a tray; once the file is on the card it becomes
 * a check. Armed fills in the accent colour, idle stays in the grey. */
static void draw_download(float cx, float cy, bool armed, bool saved,
                          u32 clr_on, u32 clr_off)
{
	const u32 clr = (armed || saved) ? clr_on : clr_off;
	if (saved) {
		ui_polyline((float[]){cx - 6.0f, cy + 0.5f, cx - 1.5f, cy + 5.5f,
		                      cx + 6.5f, cy - 4.5f}, 3, 2.4f, clr);
		return;
	}
	C2D_DrawRectSolid(cx - 1.2f, cy - 8.0f, 0.0f, 2.4f, 6.5f, clr);
	C2D_DrawTriangle(cx - 5.0f, cy - 2.5f, clr, cx + 5.0f, cy - 2.5f, clr,
	                 cx, cy + 3.0f, clr, 0.0f);
	C2D_DrawRectSolid(cx - 7.0f, cy + 4.5f, 0.0f, 14.0f, 2.0f, clr);
	C2D_DrawRectSolid(cx - 7.0f, cy + 4.5f, 0.0f, 2.0f, 3.5f, clr);
	C2D_DrawRectSolid(cx + 5.0f, cy + 4.5f, 0.0f, 2.0f, 3.5f, clr);
}

static void fmt_time(long ms, char *out, size_t out_sz)
{
	if (ms < 0)
		ms = 0;
	snprintf(out, out_sz, "%ld:%02ld", ms / 60000, (ms / 1000) % 60);
}

static void draw_header(const screen_player_args *a)
{
	const bool mute_pressed = a->pressed_id == BTN_P_MUTE;
	const bool lyr_pressed = a->pressed_id == BTN_P_LYRICS;
	const bool lib_pressed = a->pressed_id == BTN_P_LIBRARY;

	C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, HEADER_H, a->panel);

	draw_speaker(20.0f, HEADER_H / 2, a->muted,
	             press_clr(CLR_IDLE, mute_pressed, a->accent));
	tb_add_hit(a->tb, 20.0f, HEADER_H / 2, 44.0f, BTN_P_MUTE);

	/* The heart sits between mute and the actions: Me gusta for the playing
	 * track, accent-filled when it is already in the library. The track name
	 * no longer prints here -- the top screen carries it. */
	const bool like_pressed = a->pressed_id == BTN_P_LIKE;
	draw_heart(48.0f, HEADER_H / 2, a->liked, a->accent,
	           press_clr(CLR_IDLE, like_pressed, a->accent));
	tb_add_hit(a->tb, 48.0f, HEADER_H / 2, 40.0f, BTN_P_LIKE);

	/* The download button sits next to the heart, only for remote tracks:
	 * a local file is already on the card. */
	if (a->dl_show) {
		const bool dl_pressed = a->pressed_id == BTN_P_DOWNLOAD;
		draw_download(76.0f, HEADER_H / 2, a->dl_armed, a->dl_saved,
		              a->accent, press_clr(CLR_IDLE, dl_pressed, a->accent));
		tb_add_hit(a->tb, 76.0f, HEADER_H / 2, 40.0f, BTN_P_DOWNLOAD);
	}

	/* The queue button is always there: tap opens the Downloads shelf,
	 * hold shows the pending queue. A badge counts what is still waiting,
	 * and the dot pulses while a download runs. */
	{
		const bool q_pressed = a->pressed_id == BTN_P_QUEUE;
		const u32 clr = press_clr(
		    (a->dl_queue > 0 || a->dl_active) ? a->accent : CLR_IDLE,
		    q_pressed, a->accent);
		const float cx = 104.0f, cy = HEADER_H / 2;
		/* Tray with three stacked lines: the queue. */
		C2D_DrawRectSolid(cx - 7.0f, cy - 6.5f, 0.0f, 14.0f, 1.6f, clr);
		C2D_DrawRectSolid(cx - 7.0f, cy - 1.8f, 0.0f, 14.0f, 1.6f, clr);
		C2D_DrawRectSolid(cx - 7.0f, cy + 2.9f, 0.0f, 14.0f, 1.6f, clr);
		C2D_DrawRectSolid(cx - 7.0f, cy - 6.5f, 0.0f, 1.6f, 11.0f, clr);
		C2D_DrawRectSolid(cx + 5.4f, cy - 6.5f, 0.0f, 1.6f, 11.0f, clr);
		if (a->dl_active)
			ui_disc(cx + 9.5f, cy + 5.0f, 2.4f, a->accent);
		if (a->dl_queue > 0) {
			char n[8];
			snprintf(n, sizeof(n), "%d", a->dl_queue);
			ui_text(a->buf, n, cx + 10.5f, ui_baseline(cy - 3.0f, TY_MICRO),
			        TY_MICRO, 24.0f, a->accent);
		}
		tb_add_hit(a->tb, cx, cy, 40.0f, BTN_P_QUEUE);
	}

	/* Two wide hit zones on the right: lyrics and the library. The whole
	 * stretch answers, the same way the reference gives its header labels a
	 * wide area rather than just the glyph. */
	{
		float w = ui_text_width(a->buf, tr(S_LYRICS), TY_MICRO) + 4.0f;
		ui_text_tracked(a->buf, tr(S_LYRICS), 246.0f - w,
		                ui_baseline(9.0f, TY_MICRO), TY_MICRO, 1.0f,
		                press_clr(CLR_LABEL, lyr_pressed, a->accent));
		tb_add(a->tb, 186.0f, 0.0f, 66.0f, HEADER_H, BTN_P_LYRICS);
	}
	{
		float w = ui_text_width(a->buf, tr(S_LIBRARY), TY_MICRO);
		ui_text_tracked(a->buf, tr(S_LIBRARY), 296.0f - w,
		                ui_baseline(9.0f, TY_MICRO), TY_MICRO, 1.0f,
		                press_clr(CLR_LABEL, lib_pressed, a->accent));
		tri_right(302.0f, HEADER_H / 2 - 5.0f, 6.0f, 10.0f,
		          press_clr(CLR_LABEL, lib_pressed, a->accent));
		tb_add(a->tb, 252.0f, 0.0f, 68.0f, HEADER_H, BTN_P_LIBRARY);
	}
}

static void draw_centre(const screen_player_args *a)
{
	(void)a;
	/* The middle belongs to the recents shelf: the top screen already
	 * carries the track's name, artist, album and the loading state, so the
	 * equalizer placeholder that used to stand here only fights the covers. */
}

static void draw_transport(const screen_player_args *a)
{
	const float play_x = 160.0f;
	const float prev_x = play_x - (PLAY_R + CTRL_GAP + CTRL_SMALL / 2);
	const float next_x = play_x + (PLAY_R + CTRL_GAP + CTRL_SMALL / 2);

	const bool prev_pressed = a->pressed_id == BTN_P_PREV;
	const bool next_pressed = a->pressed_id == BTN_P_NEXT;

	/* Halos first, so every glyph draws on top of its own. */
	press_halo(prev_x, ROW_Y, prev_pressed, a->accent);
	press_halo(next_x, ROW_Y, next_pressed, a->accent);

	draw_prev(prev_x, ROW_Y,
	          press_clr(a->can_prev ? CLR_WHITE : CLR_IDLE, prev_pressed,
	                    a->accent));
	draw_next(next_x, ROW_Y,
	          press_clr(a->can_next ? CLR_WHITE : CLR_IDLE, next_pressed,
	                    a->accent));
	draw_playpause(play_x, ROW_Y, a->playing, a->pressed_id == BTN_P_PLAY,
	               a->accent);

	/* Centre-outward: the largest, most-tapped control wins contested
	 * pixels. */
	tb_add_hit(a->tb, play_x, ROW_Y, 44.0f, BTN_P_PLAY);
	tb_add_hit(a->tb, prev_x, ROW_Y, 44.0f, BTN_P_PREV);
	tb_add_hit(a->tb, next_x, ROW_Y, 44.0f, BTN_P_NEXT);

	/* Shuffle and repeat flank the row, like the reference client; an active
	 * mode is accent with a dot underneath. */
	const float shuf_x = 44.0f, rep_x = 276.0f;
	const u32 shuf_clr = a->shuffle ? a->accent : CLR_IDLE;
	const u32 rep_clr  = a->repeat ? a->accent : CLR_IDLE;
	draw_shuffle(shuf_x, ROW_Y,
	             press_clr(shuf_clr, a->pressed_id == BTN_P_SHUFFLE,
	                       CLR_WHITE));
	draw_repeat(rep_x, ROW_Y, a->repeat == 2,
	            press_clr(rep_clr, a->pressed_id == BTN_P_REPEAT, CLR_WHITE));
	if (a->shuffle)
		ui_disc(shuf_x, ROW_Y + 14.0f, 2.0f, a->accent);
	if (a->repeat)
		ui_disc(rep_x, ROW_Y + 14.0f, 2.0f, a->accent);
	tb_add_hit(a->tb, shuf_x, ROW_Y, 40.0f, BTN_P_SHUFFLE);
	tb_add_hit(a->tb, rep_x, ROW_Y, 40.0f, BTN_P_REPEAT);
}

static void draw_scrubber(const screen_player_args *a)
{
	char elapsed[12], total[12];
	fmt_time(a->progress_ms, elapsed, sizeof(elapsed));
	fmt_time(a->duration_ms, total, sizeof(total));

	const float base = ui_baseline(SCRUB_TIMES_Y - 12.0f, TY_ROW_SUB);
	ui_text(a->buf, elapsed, 16.0f, base, TY_ROW_SUB, 40.0f, CLR_TIME);
	{
		float w = ui_text_width(a->buf, total, TY_ROW_SUB);
		ui_text(a->buf, total, 304.0f - w, base, TY_ROW_SUB, 40.0f,
		        CLR_TIME);
	}

	C2D_DrawRectSolid(SCRUB_BAR_X, SCRUB_BAR_Y, 0.0f, SCRUB_BAR_W,
	                  UI_PROGRESS_BAR_H, CLR_TRACK);

	float frac = 0.0f;
	if (a->duration_ms > 0) {
		frac = (float)a->progress_ms / (float)a->duration_ms;
		if (frac > 1.0f)
			frac = 1.0f;
		if (frac < 0.0f)
			frac = 0.0f;
	}
	C2D_DrawRectSolid(SCRUB_BAR_X, SCRUB_BAR_Y, 0.0f, SCRUB_BAR_W * frac,
	                  UI_PROGRESS_BAR_H, CLR_WHITE);
	ui_disc(SCRUB_BAR_X + SCRUB_BAR_W * frac, SCRUB_BAR_Y + 2.0f, 5.0f,
	        a->pressed_id == BTN_P_SCRUB ? a->accent : CLR_WHITE);

	/* Generous vertical padding: the bar itself is 4px tall, untouchable. */
	tb_add(a->tb, SCRUB_BAR_X - 8.0f, SCRUB_BAR_Y - 10.0f,
	       SCRUB_BAR_W + 16.0f, 24.0f, BTN_P_SCRUB);
}

/* The shelf: the last few tracks this console played, as tappable covers.
 * Tracked locally because the Web API's recently-played endpoint sits behind
 * the same saturated quota as /v1/me. */
static void draw_shelf(const screen_player_args *a)
{
	if (a->recent_n <= 0)
		return;
	int n = a->recent_n < SHELF_MAX ? a->recent_n : SHELF_MAX;

	float w = ui_text_width(a->buf, tr(S_RECENTLY), TY_MICRO);
	ui_text_tracked(a->buf, tr(S_RECENTLY), 160.0f - w / 2,
	                ui_baseline(SHELF_LABEL_Y, TY_MICRO), TY_MICRO, 1.0f,
	                CLR_LABEL);

	const float row_w = n * SHELF_TILE + (n - 1) * SHELF_GAP;
	float x = 160.0f - row_w / 2;
	for (int i = 0; i < n; i++) {
		const C2D_Image *img = a->thumb ? a->thumb(a->recent_art[i],
		                                           a->thumb_ctx) : NULL;
		if (img)
			C2D_DrawImageAt(*img, x, SHELF_Y, 0.0f, NULL,
			                SHELF_TILE / 64.0f, SHELF_TILE / 64.0f);
		else
			C2D_DrawRectSolid(x, SHELF_Y, 0.0f, SHELF_TILE, SHELF_TILE,
			                  CLR_TILE);
		if (a->pressed_id == BTN_P_RECENT0 + i)
			C2D_DrawRectSolid(x, SHELF_Y, 0.1f, SHELF_TILE, SHELF_TILE,
			                  with_alpha(a->accent, 0x66));
		tb_add(a->tb, x, SHELF_Y, SHELF_TILE, SHELF_TILE, BTN_P_RECENT0 + i);
		x += SHELF_TILE + SHELF_GAP;
	}
}

void screen_player_draw(const screen_player_args *a)
{
	draw_header(a);
	draw_centre(a);
	draw_shelf(a);
	draw_transport(a);
	draw_scrubber(a);
}
