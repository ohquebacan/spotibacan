/* Spectrum visualizer for the top screen: 32 log-spaced bands from what the
 * speaker is playing right now, Winamp style. The audio thread hands over a
 * mono copy of each wave buffer as it queues it; the render thread reads
 * the buffer NDSP is actually playing, at its play position, so the bars
 * move with the sound rather than with the decoder seconds ahead. */
#pragma once

#include <3ds.h>
#include <stdint.h>

typedef enum { VIS_CLASSIC = 0, VIS_NEON, VIS_STYLE_COUNT } vis_style;

/* Audio thread: a wave buffer (slot = its index) was just filled. */
void vis_feed(int slot, const int16_t *stereo, unsigned frames);

/* Render thread, once a frame: analyse around play position `pos` of the
 * slot NDSP is playing (-1 when nothing plays: the bars fall to rest). */
void vis_update(int slot, unsigned pos);

/* Draws the bars over the area below the title, depth-shifted by par. */
void vis_draw(float par, u32 accent, vis_style style);
