#include "visualizer.h"

#include <citro2d.h>
#include <math.h>
#include <string.h>

#define VIS_SLOTS  4        /* == WAVE_BUFFERS in main.c */
#define VIS_FRAMES 2048     /* == FRAMES_PER_BUF */
#define FFT_N      1024
#define FFT_LOG2   10
#define BANDS      32
#define RATE       44100.0f

/* Mono copies of the queued wave buffers. A slot is only rewritten when
 * NDSP is done with it, and only the playing one is read. */
static int16_t mono[VIS_SLOTS][VIS_FRAMES];

static bool  ready;
static float window[FFT_N];
static float tw_re[FFT_N / 2], tw_im[FFT_N / 2];
static uint16_t bitrev[FFT_N];
static int   band_lo[BANDS], band_hi[BANDS];
static float band_tilt[BANDS];

static float level[BANDS];      /* 0..1, what is drawn */
static float peak[BANDS];
static float peak_vel[BANDS];
static int   peak_hold[BANDS];

void vis_feed(int slot, const int16_t *stereo, unsigned frames)
{
	if (slot < 0 || slot >= VIS_SLOTS)
		return;
	if (frames > VIS_FRAMES)
		frames = VIS_FRAMES;
	for (unsigned i = 0; i < frames; i++)
		mono[slot][i] = (int16_t)(((int)stereo[i * 2] + stereo[i * 2 + 1]) / 2);
}

static void init_tables(void)
{
	for (int i = 0; i < FFT_N; i++) {
		window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
		unsigned r = 0;
		for (int b = 0; b < FFT_LOG2; b++)
			r |= ((i >> b) & 1u) << (FFT_LOG2 - 1 - b);
		bitrev[i] = (uint16_t)r;
	}
	for (int i = 0; i < FFT_N / 2; i++) {
		tw_re[i] = cosf(-2.0f * (float)M_PI * i / FFT_N);
		tw_im[i] = sinf(-2.0f * (float)M_PI * i / FFT_N);
	}
	/* Log-spaced like hearing: 40 Hz to 16 kHz. Every band owns at least
	 * one bin, so the bottom ones do not collapse onto the same bin. */
	const float f0 = 40.0f, f1 = 16000.0f;
	int prev = 1;
	for (int k = 0; k < BANDS; k++) {
		float hi_f = f0 * powf(f1 / f0, (float)(k + 1) / BANDS);
		int hi = (int)(hi_f * FFT_N / RATE);
		if (hi <= prev)
			hi = prev + 1;
		if (hi > FFT_N / 2)
			hi = FFT_N / 2;
		band_lo[k] = prev;
		band_hi[k] = hi;
		prev = hi;
		/* Music has less energy up top; +3 dB per octave above 200 Hz keeps
		 * the right half from looking dead. */
		float fc = f0 * powf(f1 / f0, (k + 0.5f) / BANDS);
		band_tilt[k] = fc > 200.0f ? 3.0f * log2f(fc / 200.0f) : 0.0f;
	}
	ready = true;
}

void vis_update(int slot, unsigned pos)
{
	if (!ready)
		init_tables();

	float target[BANDS] = {0};
	if (slot >= 0 && slot < VIS_SLOTS) {
		static float re[FFT_N], im[FFT_N];
		int start = (int)pos - FFT_N / 2;
		if (start < 0)
			start = 0;
		if (start > VIS_FRAMES - FFT_N)
			start = VIS_FRAMES - FFT_N;
		const int16_t *src = &mono[slot][start];
		for (int i = 0; i < FFT_N; i++) {
			re[bitrev[i]] = src[i] * (1.0f / 32768.0f) * window[i];
			im[bitrev[i]] = 0.0f;
		}
		for (int size = 2; size <= FFT_N; size <<= 1) {
			const int half = size >> 1, step = FFT_N / size;
			for (int i = 0; i < FFT_N; i += size)
				for (int j = 0; j < half; j++) {
					const float wr = tw_re[j * step], wi = tw_im[j * step];
					const int a = i + j, b = a + half;
					const float xr = re[b] * wr - im[b] * wi;
					const float xi = re[b] * wi + im[b] * wr;
					re[b] = re[a] - xr;
					im[b] = im[a] - xi;
					re[a] += xr;
					im[a] += xi;
				}
		}
		/* A full-scale sine peaks near N/4 through the Hann window. */
		const float norm = 4.0f / FFT_N;
		for (int k = 0; k < BANDS; k++) {
			float m = 0.0f;
			for (int b = band_lo[k]; b < band_hi[k]; b++) {
				float v = re[b] * re[b] + im[b] * im[b];
				if (v > m)
					m = v;
			}
			float db = 10.0f * log10f(m * norm * norm + 1e-12f) + band_tilt[k];
			float t = (db + 60.0f) / 60.0f;   /* -60 dB .. 0 dB */
			target[k] = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		}
	}

	for (int k = 0; k < BANDS; k++) {
		/* Up at once, down at a steady fall: the classic analyser feel. */
		if (target[k] > level[k])
			level[k] = target[k];
		else
			level[k] -= 0.022f;
		if (level[k] < 0.0f)
			level[k] = 0.0f;
		/* Peak caps hold a moment, then drop with gravity. */
		if (level[k] >= peak[k]) {
			peak[k] = level[k];
			peak_vel[k] = 0.0f;
			peak_hold[k] = 18;
		} else if (peak_hold[k] > 0) {
			peak_hold[k]--;
		} else {
			peak_vel[k] += 0.0009f;
			peak[k] -= peak_vel[k];
			if (peak[k] < level[k])
				peak[k] = level[k];
		}
	}
}

static u32 lerp_rgb(u32 a, u32 b, float t, u8 alpha)
{
	const int ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
	const int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
	return C2D_Color32((u8)(ar + (br - ar) * t), (u8)(ag + (bg - ag) * t),
	                   (u8)(ab + (bb - ab) * t), alpha);
}

void vis_draw(float par, u32 accent, vis_style style)
{
	const float X0 = 18.0f + par * 2.0f, X1 = 382.0f + par * 2.0f;
	const float TOP = 46.0f, BASE = 196.0f;          /* bar area */
	const float H = BASE - TOP, REFL = 240.0f - BASE - 4.0f;
	const float slot_w = (X1 - X0) / BANDS;
	const float bar_w = slot_w - 2.0f;

	/* Faint baseline. */
	C2D_DrawRectSolid(X0, BASE + 1.0f, 0.2f, X1 - X0, 1.0f,
	                  C2D_Color32(0x40, 0x40, 0x40, 0xFF));

	if (style == VIS_CLASSIC) {
		/* LED blocks: green, then yellow, red at the top. */
		const int SEGS = 30;
		const float seg_h = H / SEGS;
		const u32 green = C2D_Color32(0x1E, 0xE0, 0x3A, 0xFF);
		const u32 yellow = C2D_Color32(0xF2, 0xD8, 0x10, 0xFF);
		const u32 red = C2D_Color32(0xF0, 0x36, 0x1E, 0xFF);
		for (int k = 0; k < BANDS; k++) {
			const float x = X0 + k * slot_w + 1.0f;
			const int lit = (int)(level[k] * SEGS + 0.5f);
			for (int s = 0; s < lit; s++) {
				const float t = (float)s / SEGS;
				const u32 c = t < 0.55f ? green : (t < 0.82f ? yellow : red);
				const float y = BASE - (s + 1) * seg_h;
				C2D_DrawRectSolid(x, y + 1.0f, 0.3f, bar_w, seg_h - 1.0f, c);
				/* Reflection: the first blocks again, dimmed and squashed. */
				const float ry = BASE + 3.0f + s * seg_h * 0.35f;
				if (ry < BASE + 3.0f + REFL)
					C2D_DrawRectSolid(x, ry, 0.3f, bar_w, seg_h * 0.35f - 0.5f,
					                  (c & 0x00FFFFFF) | 0x30000000);
			}
			const float py = BASE - peak[k] * H;
			if (peak[k] > 0.01f)
				C2D_DrawRectSolid(x, py - 2.0f, 0.35f, bar_w, 2.0f,
				                  C2D_Color32(0xE8, 0xE8, 0xE8, 0xFF));
		}
	} else {
		/* Neon: smooth bars in the cover's colour, brighter at the tips,
		 * mirrored below. */
		const u32 tip = lerp_rgb(accent, 0xFFFFFFFF, 0.55f, 0xFF);
		for (int k = 0; k < BANDS; k++) {
			const float x = X0 + k * slot_w + 1.0f;
			const float h = level[k] * H;
			if (h > 0.5f) {
				const u32 base_c = lerp_rgb(accent, 0xFF000000, 0.35f, 0xFF);
				const u32 top_c = lerp_rgb(base_c, tip, level[k], 0xFF);
				C2D_DrawRectangle(x, BASE - h, 0.3f, bar_w, h,
				                  top_c, top_c, base_c, base_c);
				const float rh = h * 0.35f > REFL ? REFL : h * 0.35f;
				const u32 r0 = (base_c & 0x00FFFFFF) | 0x50000000;
				const u32 r1 = (base_c & 0x00FFFFFF);
				C2D_DrawRectangle(x, BASE + 3.0f, 0.3f, bar_w, rh, r0, r0, r1, r1);
			}
			const float py = BASE - peak[k] * H;
			if (peak[k] > 0.01f)
				C2D_DrawRectSolid(x, py - 2.0f, 0.35f, bar_w, 1.5f, tip);
		}
	}
}
