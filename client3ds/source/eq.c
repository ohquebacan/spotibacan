#include "eq.h"

#include <math.h>
#include <string.h>

#define EQ_RATE   44100.0f
#define EQ_BANDS  3

/* One RBJ-cookbook biquad, direct form I, with per-channel history. */
typedef struct {
	float b0, b1, b2, a1, a2;
	float x1[2], x2[2], y1[2], y2[2];
} biquad;

typedef enum { BQ_LOWSHELF, BQ_PEAK, BQ_HIGHSHELF } bq_kind;

typedef struct {
	bq_kind kind;
	float   hz, db, q;
} band_spec;

typedef struct {
	const char *key;
	float       gain_db;   /* applied before the limiter */
	band_spec   bands[EQ_BANDS];   /* db == 0 means unused */
} preset_spec;

/* Each preset pays for part of its lift with a cut in overall level, so
 * the limiter only catches the odd peak; Boost runs hot into it. Boost
 * lifts everything about 5 dB with some low-mid warmth, but leaves the
 * deep bass (rattle on the speakers) and the top end (harsh) alone. */
static const preset_spec PRESETS[EQ_COUNT] = {
	[EQ_OFF]    = { "off",    0.0f, {{0}} },
	[EQ_BASS]   = { "bass",  -2.5f, {{ BQ_LOWSHELF, 200.0f, 5.0f, 0.7f }} },
	[EQ_TREBLE] = { "treble",-2.0f, {{ BQ_HIGHSHELF, 5000.0f, 4.0f, 0.7f }} },
	[EQ_VOCAL]  = { "vocal", -1.0f, {{ BQ_LOWSHELF, 150.0f, -3.0f, 0.7f },
	                                 { BQ_PEAK, 2500.0f, 3.0f, 1.0f }} },
	/* Boost was heard as too bright: warmth in the low mids instead of a
	 * presence bump, and the top end eased back under the added gain. */
	[EQ_BOOST]  = { "boost",  4.5f, {{ BQ_PEAK, 250.0f, 3.0f, 0.8f },
	                                 { BQ_PEAK, 3000.0f, 1.0f, 1.0f },
	                                 { BQ_HIGHSHELF, 8000.0f, -2.5f, 0.7f }} },
};

static volatile int s_want = EQ_OFF;
static int          s_have = -1;
static biquad       s_bq[EQ_BANDS];
static int          s_nbands;
static float        s_gain = 1.0f;
static float        s_lim = 1.0f;   /* limiter gain, smoothed */

void eq_set(eq_preset p)
{
	if (p >= 0 && p < EQ_COUNT)
		s_want = p;
}

eq_preset eq_get(void) { return (eq_preset)s_want; }

const char *eq_key(eq_preset p)
{
	return (p >= 0 && p < EQ_COUNT) ? PRESETS[p].key : "off";
}

eq_preset eq_from_key(const char *key)
{
	for (int i = 0; i < EQ_COUNT; i++)
		if (key && strcmp(key, PRESETS[i].key) == 0)
			return (eq_preset)i;
	return EQ_OFF;
}

static void design(biquad *f, const band_spec *b)
{
	const float A = powf(10.0f, b->db / 40.0f);
	const float w0 = 2.0f * (float)M_PI * b->hz / EQ_RATE;
	const float cw = cosf(w0), sw = sinf(w0);
	const float alpha = sw / (2.0f * b->q);
	float b0, b1, b2, a0, a1, a2;
	if (b->kind == BQ_PEAK) {
		b0 = 1.0f + alpha * A;
		b1 = -2.0f * cw;
		b2 = 1.0f - alpha * A;
		a0 = 1.0f + alpha / A;
		a1 = -2.0f * cw;
		a2 = 1.0f - alpha / A;
	} else {
		const float sa = 2.0f * sqrtf(A) * alpha;
		if (b->kind == BQ_LOWSHELF) {
			b0 = A * ((A + 1) - (A - 1) * cw + sa);
			b1 = 2 * A * ((A - 1) - (A + 1) * cw);
			b2 = A * ((A + 1) - (A - 1) * cw - sa);
			a0 = (A + 1) + (A - 1) * cw + sa;
			a1 = -2 * ((A - 1) + (A + 1) * cw);
			a2 = (A + 1) + (A - 1) * cw - sa;
		} else {
			b0 = A * ((A + 1) + (A - 1) * cw + sa);
			b1 = -2 * A * ((A - 1) + (A + 1) * cw);
			b2 = A * ((A + 1) + (A - 1) * cw - sa);
			a0 = (A + 1) - (A - 1) * cw + sa;
			a1 = 2 * ((A - 1) - (A + 1) * cw);
			a2 = (A + 1) - (A - 1) * cw - sa;
		}
	}
	memset(f, 0, sizeof(*f));
	f->b0 = b0 / a0;
	f->b1 = b1 / a0;
	f->b2 = b2 / a0;
	f->a1 = a1 / a0;
	f->a2 = a2 / a0;
}

static void rebuild(int p)
{
	const preset_spec *ps = &PRESETS[p];
	s_nbands = 0;
	for (int i = 0; i < EQ_BANDS; i++)
		if (ps->bands[i].db != 0.0f)
			design(&s_bq[s_nbands++], &ps->bands[i]);
	s_gain = powf(10.0f, ps->gain_db / 20.0f);
	s_lim = 1.0f;
	s_have = p;
}

/* The limiter rides a gain, never the waveform: when the louder channel
 * passes the ceiling the gain drops within about a millisecond and comes
 * back over ~150 ms. The old per-sample soft clipper bent every loud peak
 * of today's hot masters, which is the distortion Boost was heard with. */
#define LIM_CEIL   0.89f      /* -1 dBFS */
#define LIM_ATTACK 0.0275f    /* 1 - exp(-1 / (0.8 ms * 44.1 kHz)) */
#define LIM_REL    0.000151f  /* 1 - exp(-1 / (150 ms * 44.1 kHz)) */

void eq_process(int16_t *pcm, unsigned frames)
{
	const int want = s_want;
	if (want != s_have)
		rebuild(want);
	if (want == EQ_OFF)
		return;

	const float in_scale = s_gain / 32768.0f;
	float g = s_lim;
	for (unsigned i = 0; i < frames; i++) {
		float v[2];
		for (int ch = 0; ch < 2; ch++) {
			float x = (float)pcm[i * 2 + ch] * in_scale;
			for (int b = 0; b < s_nbands; b++) {
				biquad *f = &s_bq[b];
				float y = f->b0 * x + f->b1 * f->x1[ch] + f->b2 * f->x2[ch]
				        - f->a1 * f->y1[ch] - f->a2 * f->y2[ch];
				f->x2[ch] = f->x1[ch];
				f->x1[ch] = x;
				f->y2[ch] = f->y1[ch];
				f->y1[ch] = y;
				x = y;
			}
			v[ch] = x;
		}
		const float peak = fmaxf(fabsf(v[0]), fabsf(v[1]));
		const float target = peak > LIM_CEIL ? LIM_CEIL / peak : 1.0f;
		g += (target < g ? LIM_ATTACK : LIM_REL) * (target - g);
		for (int ch = 0; ch < 2; ch++) {
			int o = (int)(v[ch] * g * 32767.0f);
			pcm[i * 2 + ch] = (int16_t)(o > 32767 ? 32767 : (o < -32768 ? -32768 : o));
		}
	}
	s_lim = g;
}
