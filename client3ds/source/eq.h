/* Output equalizer: a few fixed presets applied to the PCM on its way from
 * the ring to NDSP, so a change is heard at once instead of after the
 * seconds of audio already queued in the ring. */
#pragma once

#include <stdint.h>

typedef enum {
	EQ_OFF = 0,
	EQ_BASS,
	EQ_TREBLE,
	EQ_VOCAL,
	EQ_BOOST,      /* louder: bass and treble lift, gain, soft limiter */
	EQ_COUNT
} eq_preset;

/* Any thread; the audio thread picks the change up on its next buffer. */
void      eq_set(eq_preset p);
eq_preset eq_get(void);
/* The key stored in spotify.cfg for a preset, and back. */
const char *eq_key(eq_preset p);
eq_preset   eq_from_key(const char *key);

/* Audio thread only: filters interleaved stereo s16 in place. */
void eq_process(int16_t *pcm, unsigned frames);
