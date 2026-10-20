#include "volume_overlay.h"

#include <stdio.h>

#include "ui.h"

/* Smartphone-style volume hint: a bare vertical line at the left edge,
 * beside the cover, no panel around it. The fill grows from the bottom and
 * the percent sits on top of the line, like the indicator a phone flashes
 * while the buttons are pressed. Fades out with `alpha` once the slider
 * settles. The overlay has no touch area: it only reports. */
#define BAR_X   10.0f
#define BAR_TOP 40.0f
#define BAR_BOT 186.0f
#define BAR_W   3.0f

static u32 color(u8 r, u8 g, u8 b, u8 alpha, u8 fade)
{
	return C2D_Color32(r, g, b, (u8)((unsigned)alpha * fade / 255));
}

void volume_overlay_draw(const volume_overlay_args *a)
{
	if (!a || !a->alpha)
		return;
	const u8 fade = a->alpha;
	const u32 track = color(0x35, 0x35, 0x35, 0xFF, fade);
	const u32 white = color(0xF4, 0xF4, 0xF4, 0xFF, fade);
	const u32 label = color(0xB3, 0xB3, 0xB3, 0xFF, fade);
	const float cx = BAR_X + BAR_W / 2.0f;

	/* Rounded caps soften the strip's ends without a panel around it. */
	C2D_DrawRectSolid(BAR_X, BAR_TOP + 1.0f, 0.0f, BAR_W, BAR_BOT - BAR_TOP - 2.0f,
	                  track);
	ui_disc(cx, BAR_TOP + 1.0f, BAR_W / 2.0f, track);
	ui_disc(cx, BAR_BOT - 1.0f, BAR_W / 2.0f, track);

	float fill = (float)a->volume_percent / 100.0f;
	if (fill > 1.0f)
		fill = 1.0f;
	if (fill > 0.0f) {
		const float h = (BAR_BOT - BAR_TOP) * fill;
		C2D_DrawRectSolid(BAR_X, BAR_BOT - h, 0.1f, BAR_W, h, white);
		ui_disc(cx, BAR_BOT - h, BAR_W / 2.0f, white);
	}
	ui_disc(cx, BAR_BOT, BAR_W / 2.0f, white);

	char value[8];
	snprintf(value, sizeof(value), "%d%%", a->volume_percent);
	const float w = ui_text_width(a->buf, value, TY_MICRO);
	ui_text(a->buf, value, cx - w / 2.0f,
	        ui_baseline(BAR_TOP - 13.0f, TY_MICRO), TY_MICRO, w, label);
}
