/* The handful of stb_vorbis declarations this probe uses.
 *
 * stb_vorbis ships as one .c file that is both header and implementation; it
 * is compiled separately in deps/, so only the prototypes are needed here.
 */
#ifndef PROBE_STB_VORBIS_H
#define PROBE_STB_VORBIS_H

typedef struct stb_vorbis stb_vorbis;

typedef struct {
	unsigned int sample_rate;
	int channels;
	unsigned int setup_memory_required;
	unsigned int setup_temp_memory_required;
	unsigned int temp_memory_required;
	int max_frame_size;
} stb_vorbis_info;

typedef struct {
	char *alloc_buffer;
	int alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;

stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len,
                                   int *error, const stb_vorbis_alloc *alloc);
/* Pulls from an open file as it decodes; `close_handle_on_close` hands the
 * FILE to the decoder, which closes it even when the open fails. */
#include <stdio.h>
stb_vorbis *stb_vorbis_open_file(FILE *f, int close_handle_on_close,
                                 int *error, const stb_vorbis_alloc *alloc);
void stb_vorbis_close(stb_vorbis *f);
void stb_vorbis_seek_start(stb_vorbis *f);
/* Sample-accurate seek; the streaming decoder checks the resulting file
 * offset against the download watermark and rolls back when it lands past
 * it. */
int stb_vorbis_seek(stb_vorbis *f, unsigned int sample_number);
/* Seeds the real length when the buffer's tail is still undownloaded zero
 * fill; without it the seek's own last-page scan poisons total_samples. */
void stb_vorbis_set_length(stb_vorbis *f, unsigned int sample_number);
stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
/* Offset into the buffer given at open: the streaming decoder compares it
 * against the download watermark before every decode call. */
unsigned int stb_vorbis_get_file_offset(stb_vorbis *f);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels,
                                             short *buffer, int num_shorts);

#endif
