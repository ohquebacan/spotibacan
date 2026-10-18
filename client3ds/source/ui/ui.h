#pragma once

#include <citro2d.h>
#include <stdbool.h>

/* Shared drawing helpers and the type scale.
 *
 * Type is specified in CSS pixels, the way the reference mockup talks about
 * it. citro2d's text scale is a float multiplier on the system font, so the
 * mapping has to be derived from the font's own metrics rather than guessed -
 * and the system font differs by region and firmware, so it cannot be
 * hardcoded either. ui_init() reads fontGetInfo() once and fills the table.
 */

/* Type roles, named for what the mockup calls them rather than for a size. */
typedef enum {
	TY_TITLE_L = 0, /* 34px - art-hidden title */
	TY_TITLE,       /* 21px - title beside cover */
	TY_ARTIST_L,    /* 19px - art-hidden artist */
	TY_ALBUM_L,     /* 15px - art-hidden album */
	TY_ARTIST,      /* 14px */
	TY_ROW_NAME,    /* 14px - list row name, header */
	TY_ALBUM,       /* 12px */
	TY_ROW_SUB,     /* 12px - list row subtitle, scrubber times */
	TY_MICRO,       /* tracked mono-ish labels; see ui_micro_px() */
	TY_COUNT
} type_role;

#define UI_PROGRESS_BAR_H 4.0f

/* Call once after C2D_Init. */
void ui_init(void);
void ui_exit(void);

/* Scale multiplier for a role, and the CSS px it was derived from. */
float ui_scale(type_role r);
float ui_px(type_role r);

/* The mockup asks for 7px micro-labels, which sit below the legibility floor:
 * the glyph atlas is minified ~4x and the filter turns it to mush. This
 * returns the size actually used so the choice lives in one place while it is
 * being evaluated. */
float ui_micro_px(void);
void  ui_set_micro_px(float px);

/* Baseline of the first line if its box starts at `top`. Gaps read as visual
 * gaps, so stack by baseline rather than by line box (which is taller than
 * the em and would space everything too loosely). */
float ui_baseline(float top, type_role r);

/* Draw text, truncating with an ellipsis if it exceeds maxw. */
void ui_text(C2D_TextBuf buf, const char *s, float x, float y, type_role r,
             float maxw, u32 clr);

/* Draw text broken across up to maxlines lines at word boundaries, and return
 * the baseline of the last line drawn. `leading` multiplies ui_px(r) to get
 * the baseline step. Unlike ui_text this is for prose that must be read in
 * full - error messages - rather than for a name that may be ellipsised.
 * Text still remaining on the last permitted line is ellipsised as usual. */
float ui_text_wrapped(C2D_TextBuf buf, const char *s, float x, float y,
                      type_role r, float maxw, u32 clr, int maxlines,
                      float leading);

/* Draw one case-insensitive matched substring in a second colour. Falls back
 * to ui_text when needle is empty or absent. */
void ui_text_highlight(C2D_TextBuf buf, const char *s, const char *needle,
                       float x, float y, type_role r, float maxw, u32 clr,
                       u32 highlight_clr);

/* Draw text with per-glyph letter-spacing, for tracked micro labels
 * ("ME GUSTA", the account name). citro2d has no tracking parameter, so this
 * advances glyph by glyph. Only worth it for short labels. */
void ui_text_tracked(C2D_TextBuf buf, const char *s, float x, float y,
                     type_role r, float tracking_px, u32 clr);

/* Width the string would occupy, for right-aligning and centring. */
float ui_text_width(C2D_TextBuf buf, const char *s, type_role r);

/* Trims s in place (on a codepoint boundary, ending in "..") until it fits
 * maxw at a raw citro2d scale. Measures on a private buffer, so it costs
 * the frame's text arena nothing. */
void ui_fit_scaled(C2D_TextBuf fallback, char *s, size_t cap, float sc,
                   float maxw);

/* Depth for the ui_* primitives (text, discs, polylines). They all draw at
 * z=0 by default; a band painted over scrolling content (the list header)
 * needs to sit above the images that content draws at z=0.1. Reset to 0
 * after the overpainting block. */
void ui_depth(float z);

/* Filled circle. citro2d has no circle primitive, so this is a triangle fan
 * whose segment count scales with the radius to hold the chord error under a
 * quarter pixel. There is no MSAA on this target, so the outermost pixel is a
 * ring that fades to alpha 0 and does the anti-aliasing by hand. */
void ui_disc(float cx, float cy, float r, u32 clr);

/* Anti-aliased polyline. C2D_DrawLine is a hard-edged quad, so a curve built
 * from it stair-steps along every segment and notches at the joints. This lays
 * one continuous strip with edges that fade to alpha 0, and shares the joint
 * vertices between segments so the seams disappear. `pts` is x,y interleaved. */
void ui_polyline(const float *pts, int count, float thickness, u32 clr);

/* Darken cover art and draw the four-bar current-playback indicator. Active
 * playback animates slowly; paused playback remains at a fixed silhouette.
 * The bars take the accent colour so they follow the settings choice. */
void ui_now_playing_badge(float x, float y, float size, bool playing,
                          unsigned animation_ms, u32 accent);

/* Bottom-screen playback progress used by non-player views. The player has
 * its own interactive scrubber instead. */
void ui_progress_bar(long elapsed_ms, long duration_ms);
