/*
 * Temporal video denoise filter (CPU, async frame path).
 *
 * Recursive per-pixel temporal lowpass using a similarity-weighted
 * coefficient LUT, following the temporal component of the hqdn3d
 * algorithm (Daniel Moreno, as found in FFmpeg's vf_hqdn3d.c,
 * LGPL-2.1-or-later; spatial passes intentionally omitted). Filtering
 * is strictly pixel-wise against a 16-bit accumulator of the previous
 * output frame, which makes it layout-agnostic: planar, semi-planar
 * (NV12) and packed 4:2:2 formats are all handled byte-wise, with the
 * luma/chroma LUT chosen per byte position.
 *
 * The similarity curve makes the filter motion-adaptive on its own:
 * large frame-to-frame differences get a near-zero coefficient and
 * pass through untouched, so moving content is not smeared while
 * static-area sensor noise is flattened (which is what actually costs
 * bitrate in the encoder).
 */

#include <obs-module.h>
#include <util/threading.h>
#include <math.h>

#define S_LUMA_STRENGTH "luma_strength"
#define S_CHROMA_STRENGTH "chroma_strength"

#define TEXT_LUMA_STRENGTH obs_module_text("TemporalDenoise.LumaStrength")
#define TEXT_CHROMA_STRENGTH obs_module_text("TemporalDenoise.ChromaStrength")

#define LUT_BITS 4
#define LUT_CENTER (256 << LUT_BITS)
#define LUT_SIZE (LUT_CENTER * 2)

struct temporal_denoise_data {
	obs_source_t *context;

	pthread_mutex_t mutex;

	int16_t lut_luma[LUT_SIZE];
	int16_t lut_chroma[LUT_SIZE];

	/* 16-bit accumulator (8 fractional bits) per plane byte */
	uint16_t *state[MAX_AV_PLANES];
	size_t state_size[MAX_AV_PLANES];

	uint32_t width;
	uint32_t height;
	enum video_format format;
	bool warned_format;
};

static const char *temporal_denoise_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("TemporalDenoiseFilter");
}

static void precalc_lut(double strength, int16_t *lut)
{
	const double dist25 = fmin(strength, 252.0);
	const double gamma = log(0.25) / log(1.0 - dist25 / 255.0 - 0.00001);

	for (int i = -LUT_CENTER; i < LUT_CENTER; i++) {
		const double f = ((i << (9 - LUT_BITS)) + (1 << (8 - LUT_BITS)) - 1) / 512.0;
		const double simil = fmax(0.0, 1.0 - fabs(f) / 255.0);
		lut[LUT_CENTER + i] = (int16_t)lrint(pow(simil, gamma) * 256.0 * f);
	}
}

static void temporal_denoise_update(void *data, obs_data_t *settings)
{
	struct temporal_denoise_data *filter = data;

	pthread_mutex_lock(&filter->mutex);
	precalc_lut(obs_data_get_double(settings, S_LUMA_STRENGTH), filter->lut_luma);
	precalc_lut(obs_data_get_double(settings, S_CHROMA_STRENGTH), filter->lut_chroma);
	pthread_mutex_unlock(&filter->mutex);
}

static void *temporal_denoise_create(obs_data_t *settings, obs_source_t *context)
{
	struct temporal_denoise_data *filter = bzalloc(sizeof(*filter));

	filter->context = context;
	pthread_mutex_init(&filter->mutex, NULL);
	temporal_denoise_update(filter, settings);

	return filter;
}

static void free_state(struct temporal_denoise_data *filter)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		bfree(filter->state[i]);
		filter->state[i] = NULL;
		filter->state_size[i] = 0;
	}
}

static void temporal_denoise_destroy(void *data)
{
	struct temporal_denoise_data *filter = data;

	free_state(filter);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
}

static obs_properties_t *temporal_denoise_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_float_slider(props, S_LUMA_STRENGTH, TEXT_LUMA_STRENGTH, 0.0, 20.0, 0.5);
	obs_properties_add_float_slider(props, S_CHROMA_STRENGTH, TEXT_CHROMA_STRENGTH, 0.0, 20.0, 0.5);

	UNUSED_PARAMETER(data);
	return props;
}

static void temporal_denoise_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_LUMA_STRENGTH, 6.0);
	obs_data_set_default_double(settings, S_CHROMA_STRENGTH, 4.5);
}

static inline uint8_t lowpass8(uint16_t *ant, uint8_t cur, const int16_t *lut)
{
	const int cur16 = cur << 8;
	const int d = (*ant - cur16) >> (8 - LUT_BITS);
	int out = cur16 + lut[LUT_CENTER + d];

	/* LUT coefficients are computed for the midpoint of each
	 * difference bucket, so the result can overshoot the accumulator
	 * slightly near the extremes; clamp to the representable range
	 * (255 << 8) or the byte conversion below wraps around */
	if (out < 0)
		out = 0;
	else if (out > (255 << 8))
		out = 255 << 8;

	*ant = (uint16_t)out;
	return (uint8_t)((out + 0x7F) >> 8);
}

/* Filter `rows` rows of `row_bytes` bytes with a single LUT. */
static void denoise_plane(uint8_t *data, uint16_t *ant, uint32_t rows, uint32_t row_bytes, uint32_t linesize,
			  const int16_t *lut)
{
	for (uint32_t y = 0; y < rows; y++) {
		uint8_t *px = data + (size_t)y * linesize;
		uint16_t *st = ant + (size_t)y * row_bytes;

		for (uint32_t x = 0; x < row_bytes; x++)
			px[x] = lowpass8(&st[x], px[x], lut);
	}
}

/* Filter packed 4:2:2 rows where luma sits at every second byte,
 * starting at `luma_offset` (0 for YUY2/YVYU, 1 for UYVY). */
static void denoise_packed422(uint8_t *data, uint16_t *ant, uint32_t rows, uint32_t row_bytes, uint32_t linesize,
			      const int16_t *lut_luma, const int16_t *lut_chroma, uint32_t luma_offset)
{
	for (uint32_t y = 0; y < rows; y++) {
		uint8_t *px = data + (size_t)y * linesize;
		uint16_t *st = ant + (size_t)y * row_bytes;

		for (uint32_t x = 0; x < row_bytes; x++) {
			const int16_t *lut = ((x & 1) == luma_offset) ? lut_luma : lut_chroma;
			px[x] = lowpass8(&st[x], px[x], lut);
		}
	}
}

struct plane_geometry {
	uint32_t rows;
	uint32_t row_bytes;
	bool chroma;
};

/* Returns the number of planes, 0 if the format is unsupported.
 * `packed_luma_offset` is set >= 0 for packed 4:2:2 formats. */
static size_t get_plane_geometry(const struct obs_source_frame *frame, struct plane_geometry geo[MAX_AV_PLANES],
				 int *packed_luma_offset)
{
	const uint32_t w = frame->width;
	const uint32_t h = frame->height;
	const uint32_t cw = (w + 1) / 2;
	const uint32_t ch = (h + 1) / 2;

	*packed_luma_offset = -1;

	switch (frame->format) {
	case VIDEO_FORMAT_I420:
		geo[0] = (struct plane_geometry){h, w, false};
		geo[1] = (struct plane_geometry){ch, cw, true};
		geo[2] = (struct plane_geometry){ch, cw, true};
		return 3;
	case VIDEO_FORMAT_I422:
		geo[0] = (struct plane_geometry){h, w, false};
		geo[1] = (struct plane_geometry){h, cw, true};
		geo[2] = (struct plane_geometry){h, cw, true};
		return 3;
	case VIDEO_FORMAT_I444:
		geo[0] = (struct plane_geometry){h, w, false};
		geo[1] = (struct plane_geometry){h, w, true};
		geo[2] = (struct plane_geometry){h, w, true};
		return 3;
	case VIDEO_FORMAT_NV12:
		geo[0] = (struct plane_geometry){h, w, false};
		/* interleaved UV: pixel-wise temporal filtering never
		 * mixes neighboring bytes, so U/V interleave is fine */
		geo[1] = (struct plane_geometry){ch, cw * 2, true};
		return 2;
	case VIDEO_FORMAT_Y800:
		geo[0] = (struct plane_geometry){h, w, false};
		return 1;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
		geo[0] = (struct plane_geometry){h, w * 2, false};
		*packed_luma_offset = 0;
		return 1;
	case VIDEO_FORMAT_UYVY:
		geo[0] = (struct plane_geometry){h, w * 2, false};
		*packed_luma_offset = 1;
		return 1;
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		/* per-byte temporal filtering works on RGB too; the
		 * constant alpha/padding byte is a no-op */
		geo[0] = (struct plane_geometry){h, w * 4, false};
		return 1;
	default:
		return 0;
	}
}

static struct obs_source_frame *temporal_denoise_video(void *data, struct obs_source_frame *frame)
{
	struct temporal_denoise_data *filter = data;
	struct plane_geometry geo[MAX_AV_PLANES];
	int packed_luma_offset;

	const size_t planes = get_plane_geometry(frame, geo, &packed_luma_offset);
	if (!planes) {
		if (!filter->warned_format) {
			blog(LOG_WARNING, "[temporal denoise: '%s'] unsupported format %d, passing through",
			     obs_source_get_name(filter->context), (int)frame->format);
			filter->warned_format = true;
		}
		return frame;
	}

	pthread_mutex_lock(&filter->mutex);

	const bool reset = frame->width != filter->width || frame->height != filter->height ||
			   frame->format != filter->format;

	if (reset) {
		filter->width = frame->width;
		filter->height = frame->height;
		filter->format = frame->format;
		filter->warned_format = false;
	}

	for (size_t i = 0; i < planes; i++) {
		const size_t size = (size_t)geo[i].rows * geo[i].row_bytes * sizeof(uint16_t);

		if (reset || filter->state_size[i] != size) {
			bfree(filter->state[i]);
			filter->state[i] = bmalloc(size);
			filter->state_size[i] = size;

			/* seed the accumulator with the current frame:
			 * the first frame passes through unchanged */
			for (uint32_t y = 0; y < geo[i].rows; y++) {
				const uint8_t *px = frame->data[i] + (size_t)y * frame->linesize[i];
				uint16_t *st = filter->state[i] + (size_t)y * geo[i].row_bytes;

				for (uint32_t x = 0; x < geo[i].row_bytes; x++)
					st[x] = (uint16_t)(px[x] << 8);
			}
			continue;
		}

		if (packed_luma_offset >= 0) {
			denoise_packed422(frame->data[i], filter->state[i], geo[i].rows, geo[i].row_bytes,
					  frame->linesize[i], filter->lut_luma, filter->lut_chroma,
					  (uint32_t)packed_luma_offset);
		} else {
			denoise_plane(frame->data[i], filter->state[i], geo[i].rows, geo[i].row_bytes,
				      frame->linesize[i], geo[i].chroma ? filter->lut_chroma : filter->lut_luma);
		}
	}

	pthread_mutex_unlock(&filter->mutex);

	return frame;
}

struct obs_source_info temporal_denoise_filter = {
	.id = "temporal_denoise_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = temporal_denoise_name,
	.create = temporal_denoise_create,
	.destroy = temporal_denoise_destroy,
	.update = temporal_denoise_update,
	.get_defaults = temporal_denoise_defaults,
	.get_properties = temporal_denoise_properties,
	.filter_video = temporal_denoise_video,
};
