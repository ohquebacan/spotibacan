#include "volume_overlay.h"

#include <stdio.h>

#include "ui.h"

/* The overlay lives on the top screen now: on the bottom one it covered the
 * transport controls it only reports on. A slim pill across the upper edge
 * stays clear of both the cover art and the text column below it: the art
 * and the title start at y=30, so 4..26 keeps a clean gap. */
#define TOP_W   400.0f
#define PANEL_W 210.0f
#define PANEL_H 22.0f
#define PANEL_X ((TOP_W - PANEL_W) / 2.0f)
#define PANEL_Y 4.0f

static u32 color(u8 r, u8 g, u8 b, u8 alpha, u8 fade)
{
	return C2D_Color32(r, g, b, (u8)((unsigned)alpha * fade / 255));
}

static void rounded_rect(float x, float y, float w, float h, float r, u32 clr)
{
	C2D_DrawRectSolid(x + r, y, 0.0f, w - 2.0f * r, h, clr);
	C2D_DrawRectSolid(x, y + r, 0.0f, w, h - 2.0f * r, clr);
	ui_disc(x + r, y + r, r, clr);
	ui_disc(x + w - r, y + r, r, clr);
	ui_disc(x + r, y + h - r, r, clr);
	ui_disc(x + w - r, y + h - r, r, clr);
}

/* The arc is a quadratic Bezier stroked as one anti-aliased strip: eight
 * chords track it to within 0.05px, and one strip has no joints to notch. */
#define WAVE_SEGMENTS 8

static void draw_wave(float x, float cy, float radius, float bulge, u32 clr)
{
	float pts[(WAVE_SEGMENTS + 1) * 2];
	for (int i = 0; i <= WAVE_SEGMENTS; i++) {
		const float t = (float)i / (float)WAVE_SEGMENTS;
		const float inv = 1.0f - t;
		pts[i * 2] = inv * inv * x + 2.0f * inv * t * (x + bulge) + t * t * x;
		pts[i * 2 + 1] = inv * inv * (cy - radius) +
		                 2.0f * inv * t * cy + t * t * (cy + radius);
	}
	ui_polyline(pts, WAVE_SEGMENTS + 1, 2.0f, clr);
}

static void draw_speaker(float x, float cy, bool waves, u32 clr)
{
	C2D_DrawRectSolid(x, cy - 3.5f, 0.0f, 4.0f, 7.0f, clr);
	C2D_DrawTriangle(x + 4.0f, cy - 3.5f, clr, x + 9.0f, cy - 8.0f, clr,
	                 x + 9.0f, cy + 8.0f, clr, 0.0f);
	C2D_DrawTriangle(x + 4.0f, cy - 3.5f, clr, x + 9.0f, cy + 8.0f, clr,
	                 x + 4.0f, cy + 3.5f, clr, 0.0f);

	if (waves) {
		draw_wave(x + 12.0f, cy, 4.5f, 3.2f, clr);
		draw_wave(x + 14.5f, cy, 7.5f, 4.8f, clr);
	} else {
		const float down[4] = {x + 13.0f, cy - 4.5f, x + 19.0f, cy + 4.5f};
		const float up[4] = {x + 19.0f, cy - 4.5f, x + 13.0f, cy + 4.5f};
		ui_polyline(down, 2, 2.0f, clr);
		ui_polyline(up, 2, 2.0f, clr);
	}
}

void volume_overlay_draw(const volume_overlay_args *a)
{
	if (!a || !a->alpha)
		return;
	const u8 fade = a->alpha;
	const u32 shadow = color(0, 0, 0, 0xA8, fade);
	const u32 border = color(0x38, 0x38, 0x38, 0xFF, fade);
	const u32 panel = color(0x1B, 0x1B, 0x1B, 0xFF, fade);
	const u32 white = color(0xF4, 0xF4, 0xF4, 0xFF, fade);
	const u32 green = color(0x1D, 0xB9, 0x54, 0xFF, fade);
	const u32 track = color(0x35, 0x35, 0x35, 0xFF, fade);

	const float cy = PANEL_Y + PANEL_H / 2.0f;

	rounded_rect(PANEL_X + 2.0f, PANEL_Y + 3.0f, PANEL_W, PANEL_H, 9.0f,
	             shadow);
	rounded_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 9.0f, border);
	rounded_rect(PANEL_X + 1.0f, PANEL_Y + 1.0f, PANEL_W - 2.0f,
	             PANEL_H - 2.0f, 8.0f, panel);

	draw_speaker(PANEL_X + 14.0f, cy, a->volume_percent > 0, white);

	char value[8];
	snprintf(value, sizeof(value), "%d%%", a->volume_percent);
	const float value_w = ui_text_width(a->buf, value, TY_ROW_SUB);
	ui_text(a->buf, value, PANEL_X + PANEL_W - 12.0f - value_w,
	        ui_baseline(cy - 6.0f, TY_ROW_SUB), TY_ROW_SUB, value_w, white);

	/* One continuous bar between the speaker and the percentage: simpler
	 * than segments and it matches the scrubber on the same screen. */
	const float bar_x = PANEL_X + 40.0f;
	/* Right side budget: 10px gap, the value text, 12px to the pill edge. */
	const float bar_w = PANEL_W - 40.0f - 10.0f - value_w - 12.0f;
	float fill = (float)a->volume_percent / 100.0f;
	if (fill > 1.0f)
		fill = 1.0f;
	C2D_DrawRectSolid(bar_x, cy - 2.0f, 0.0f, bar_w, 4.0f, track);
	if (fill > 0.0f)
		C2D_DrawRectSolid(bar_x, cy - 2.0f, 0.1f, bar_w * fill, 4.0f, green);
}
