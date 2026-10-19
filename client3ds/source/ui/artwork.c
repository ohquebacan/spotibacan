#include "artwork.h"

#include "cfg3ds.h"
#include "http3ds.h"
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Covers are JPEGs of a few tens of KB; the cap only guards against a
 * broken or hostile edge. */
#define ART_MAX_BYTES (1024 * 1024)

/* Box-average downscale: sources are 64/300/640px and targets 48/128,
 * so every output pixel averages a small source patch. */
static void downscale_rgb565(const unsigned char *rgb, int w, int h, int px,
                             uint8_t *out_rgb565)
{
	for (int y = 0; y < px; y++) {
		for (int x = 0; x < px; x++) {
			int x0 = x * w / px, x1 = (x + 1) * w / px;
			int y0 = y * h / px, y1 = (y + 1) * h / px;
			if (x1 <= x0)
				x1 = x0 + 1;
			if (y1 <= y0)
				y1 = y0 + 1;
			unsigned rs = 0, gs = 0, bs = 0, n = 0;
			for (int sy = y0; sy < y1 && sy < h; sy++)
				for (int sx = x0; sx < x1 && sx < w; sx++) {
					const unsigned char *p = rgb + (sy * w + sx) * 3;
					rs += p[0];
					gs += p[1];
					bs += p[2];
					n++;
				}
			if (n) {
				rs /= n;
				gs /= n;
				bs /= n;
			}
			uint16_t v = (uint16_t)(((rs >> 3) << 11) | ((gs >> 2) << 5) |
			                        (bs >> 3));
			out_rgb565[(y * px + x) * 2] = (uint8_t)(v & 0xFF);
			out_rgb565[(y * px + x) * 2 + 1] = (uint8_t)(v >> 8);
		}
	}
}

bool artwork_fetch_decode(const char *url, int px, uint8_t *out_rgb565,
                          char *err, size_t errlen)
{
	if (!url || !*url) {
		snprintf(err, errlen, "no art url");
		return false;
	}

	/* A cover saved beside a download lives on the SD: read it from there
	 * instead of the network, which is the whole point of offline mode. */
	if (strncmp(url, "file:", 5) == 0) {
		char path[400];
		cfg_dir(path, sizeof(path));
		snprintf(path + strlen(path), sizeof(path) - strlen(path),
		         "music/%s", url + 5);
		return artwork_load_file(path, px, out_rgb565, err, errlen);
	}

	http_reply r;
	memset(&r, 0, sizeof(r));
	/* Covers all come from one host, so the art worker (the only caller)
	 * keeps one keep-alive link to it. A fresh handshake per thumbnail was
	 * dozens of handshakes per scrolled list, starving the song playing. */
	static http_session *img_sess;
	static const char IMG_PREFIX[] = "https://i.scdn.co/";
	int rc;
	if (strncmp(url, IMG_PREFIX, sizeof(IMG_PREFIX) - 1) == 0) {
		if (!img_sess)
			img_sess = http_session_open("i.scdn.co");
		rc = img_sess
		     ? http_session_request(img_sess, url + sizeof(IMG_PREFIX) - 2,
		                            "GET", NULL, NULL, NULL, NULL, 0,
		                            ART_MAX_BYTES, &r)
		     : -1;
	} else {
		rc = http_get_url(url, ART_MAX_BYTES, &r);
	}
	if (rc != 0 || (r.status != 200 && r.status != 206)) {
		snprintf(err, errlen, "%s", r.err[0] ? r.err : "cover fetch failed");
		http_reply_free(&r);
		return false;
	}

	int w = 0, h = 0, ch = 0;
	unsigned char *rgb =
	    stbi_load_from_memory((const unsigned char *)r.body, (int)r.len,
	                          &w, &h, &ch, 3);
	http_reply_free(&r);
	if (!rgb) {
		snprintf(err, errlen, "jpeg decode failed");
		return false;
	}

	downscale_rgb565(rgb, w, h, px, out_rgb565);
	stbi_image_free(rgb);
	return true;
}

bool artwork_load_file(const char *filepath, int px, uint8_t *out_rgb565,
                       char *err, size_t errlen)
{
	/* The user's own avatar file, PNG or JPEG: the Web API rate-limits the
	 * profile fetch so hard that a file on the card is the reliable path.
	 * stb is built without stdio, so the file is slurped first. */
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		snprintf(err, errlen, "cannot open file");
		return false;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > 4 * 1024 * 1024) {
		fclose(f);
		snprintf(err, errlen, "bad file size");
		return false;
	}
	unsigned char *data = malloc((size_t)sz);
	if (!data || fread(data, 1, (size_t)sz, f) != (size_t)sz) {
		free(data);
		fclose(f);
		snprintf(err, errlen, "cannot read file");
		return false;
	}
	fclose(f);

	int w = 0, h = 0, ch = 0;
	unsigned char *rgb = stbi_load_from_memory(data, (int)sz, &w, &h, &ch, 3);
	free(data);
	if (!rgb) {
		snprintf(err, errlen, "image decode failed");
		return false;
	}
	downscale_rgb565(rgb, w, h, px, out_rgb565);
	stbi_image_free(rgb);
	return true;
}

void artwork_tile_rgb565(const uint8_t *src, uint8_t *dst, int dim)
{
	const int tiles_per_row = dim / 8;
	for (int y = 0; y < dim; y++) {
		for (int x = 0; x < dim; x++) {
			int tile = (y / 8) * tiles_per_row + (x / 8);
			/* Interleave the low 3 bits of x and y inside the tile. */
			int within = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
			             ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
			uint16_t v;
			memcpy(&v, src + (y * dim + x) * 2, 2);
			memcpy(dst + (tile * 64 + within) * 2, &v, 2);
		}
	}
}
