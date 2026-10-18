/* stb_vorbis plus the one hook the streaming decoder needs.
 *
 * Upstream's patched copy never shipped; set_length is the only symbol
 * native.c uses that stock stb_vorbis lacks. Seeding total_samples keeps
 * stb_vorbis_stream_length_in_samples (and seek) from scanning the tail of
 * a buffer that is still zero fill while the download runs. */
#include "stb_vorbis.c"

void stb_vorbis_set_length(stb_vorbis *f, unsigned int sample_number)
{
	f->total_samples = sample_number;
	/* The scan this skips is also what fills p_last, the right edge of
	 * seek_to_sample_coarse's bisection. Left zeroed, the first seek
	 * tripped its asserts (or looped) and hung the app. A synthetic last
	 * page at the end of the buffer, holding the known length, gives the
	 * interpolation the right slope; probes into the undownloaded tail
	 * find no page and fail the seek, which the player handles by moving
	 * the download there. */
	f->p_last.page_start = f->stream_len > 1 ? f->stream_len - 1 : 0;
	f->p_last.page_end = f->stream_len;
	f->p_last.last_decoded_sample = sample_number;
}
