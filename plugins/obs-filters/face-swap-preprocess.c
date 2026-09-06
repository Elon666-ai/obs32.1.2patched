#include "face-swap-preprocess.h"

#include <math.h>
#include <string.h>

static inline uint8_t clamp_byte(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (uint8_t)value;
}

static inline uint8_t sample_plane(const uint8_t *data, uint32_t stride, uint32_t x, uint32_t y)
{
	return data[(size_t)y * stride + x];
}

static inline void yuv_to_rgb(uint8_t *out, int y, int u, int v, const float matrix[16])
{
	/* OBS stores the same 3x4 YUV conversion matrix used by its shader:
	 * each row is R/G/B and the fourth value is an offset. */
	const float yf = (float)y / 255.0f;
	const float uf = (float)u / 255.0f;
	const float vf = (float)v / 255.0f;
	const int r = (int)((matrix[0] * yf + matrix[1] * uf + matrix[2] * vf + matrix[3]) * 255.0f + 0.5f);
	const int g = (int)((matrix[4] * yf + matrix[5] * uf + matrix[6] * vf + matrix[7]) * 255.0f + 0.5f);
	const int b = (int)((matrix[8] * yf + matrix[9] * uf + matrix[10] * vf + matrix[11]) * 255.0f + 0.5f);

	out[0] = clamp_byte(r);
	out[1] = clamp_byte(g);
	out[2] = clamp_byte(b);
}

bool face_swap_frame_to_rgb(uint8_t *rgb, size_t rgb_size, const struct obs_source_frame *frame)
{
	if (!rgb || !frame || frame->width == 0 || frame->height == 0 ||
	    frame->width > SIZE_MAX / 3 / frame->height ||
	    rgb_size < (size_t)frame->width * frame->height * 3)
		return false;

	if (!frame->data[0])
		return false;

	for (uint32_t y = 0; y < frame->height; y++) {
		const uint32_t src_y = frame->flip ? frame->height - 1 - y : y;
		for (uint32_t x = 0; x < frame->width; x++) {
			int yy;
			int u;
			int v;

			switch (frame->format) {
			case VIDEO_FORMAT_I420:
				if (!frame->data[1] || !frame->data[2] || frame->linesize[0] < frame->width ||
				    frame->linesize[1] < (frame->width + 1) / 2 ||
				    frame->linesize[2] < (frame->width + 1) / 2)
					return false;
				yy = sample_plane(frame->data[0], frame->linesize[0], x, src_y);
				u = sample_plane(frame->data[1], frame->linesize[1], x / 2, src_y / 2);
				v = sample_plane(frame->data[2], frame->linesize[2], x / 2, src_y / 2);
				break;
			case VIDEO_FORMAT_NV12:
				if (!frame->data[1] || frame->linesize[0] < frame->width ||
				    frame->linesize[1] < ((frame->width + 1) / 2) * 2)
					return false;
				yy = sample_plane(frame->data[0], frame->linesize[0], x, src_y);
				u = sample_plane(frame->data[1], frame->linesize[1], (x / 2) * 2, src_y / 2);
				v = sample_plane(frame->data[1], frame->linesize[1], (x / 2) * 2 + 1, src_y / 2);
				break;
			case VIDEO_FORMAT_YUY2:
			case VIDEO_FORMAT_YVYU:
			case VIDEO_FORMAT_UYVY: {
				if ((frame->width & 1) != 0 || frame->linesize[0] < frame->width * 2)
					return false;
				const uint8_t *row = frame->data[0] + (size_t)src_y * frame->linesize[0];
				const uint32_t pair = (x / 2) * 4;
				if (frame->format == VIDEO_FORMAT_UYVY) {
					u = row[pair];
					v = row[pair + 2];
					yy = row[pair + 1 + (x & 1) * 2];
				} else {
					yy = row[pair + (x & 1) * 2];
					u = row[pair + (frame->format == VIDEO_FORMAT_YVYU ? 3 : 1)];
					v = row[pair + (frame->format == VIDEO_FORMAT_YVYU ? 1 : 3)];
				}
				break;
			}
			default:
				return false;
			}

			yuv_to_rgb(rgb + ((size_t)y * frame->width + x) * 3, yy, u, v, frame->color_matrix);
		}
	}

	return true;
}

static bool invert_color_matrix(const float matrix[16], float inverse[9])
{
	const float a = matrix[0], b = matrix[1], c = matrix[2];
	const float d = matrix[4], e = matrix[5], f = matrix[6];
	const float g = matrix[8], h = matrix[9], i = matrix[10];
	const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (!isfinite(determinant) || fabsf(determinant) < 1e-8f)
		return false;
	const float scale = 1.0f / determinant;
	inverse[0] = (e * i - f * h) * scale;
	inverse[1] = (c * h - b * i) * scale;
	inverse[2] = (b * f - c * e) * scale;
	inverse[3] = (f * g - d * i) * scale;
	inverse[4] = (a * i - c * g) * scale;
	inverse[5] = (c * d - a * f) * scale;
	inverse[6] = (d * h - e * g) * scale;
	inverse[7] = (b * g - a * h) * scale;
	inverse[8] = (a * e - b * d) * scale;
	return true;
}

static void rgb_to_yuv(const uint8_t *rgb, const float matrix[16], const float inverse[9], float yuv[3])
{
	const float r = (float)rgb[0] / 255.0f - matrix[3];
	const float g = (float)rgb[1] / 255.0f - matrix[7];
	const float b = (float)rgb[2] / 255.0f - matrix[11];
	yuv[0] = (inverse[0] * r + inverse[1] * g + inverse[2] * b) * 255.0f;
	yuv[1] = (inverse[3] * r + inverse[4] * g + inverse[5] * b) * 255.0f;
	yuv[2] = (inverse[6] * r + inverse[7] * g + inverse[8] * b) * 255.0f;
}

bool face_swap_rgb_to_frame(struct obs_source_frame *frame, const uint8_t *rgb, size_t rgb_size)
{
	if (!frame || !rgb || !frame->width || !frame->height || frame->width > SIZE_MAX / 3 / frame->height ||
	    rgb_size < (size_t)frame->width * frame->height * 3 || !frame->data[0])
		return false;
	float inverse[9];
	if (!invert_color_matrix(frame->color_matrix, inverse))
		return false;
	const bool packed = frame->format == VIDEO_FORMAT_YUY2 || frame->format == VIDEO_FORMAT_YVYU ||
			    frame->format == VIDEO_FORMAT_UYVY;
	if (packed && ((frame->width & 1) || frame->linesize[0] < frame->width * 2))
		return false;

	for (uint32_t y = 0; y < frame->height; y++) {
		const uint32_t dst_y = frame->flip ? frame->height - 1 - y : y;
		if (packed) {
			uint8_t *row = frame->data[0] + (size_t)dst_y * frame->linesize[0];
			for (uint32_t x = 0; x < frame->width; x += 2) {
				float first[3], second[3];
				rgb_to_yuv(rgb + ((size_t)y * frame->width + x) * 3, frame->color_matrix, inverse, first);
				rgb_to_yuv(rgb + ((size_t)y * frame->width + x + 1) * 3, frame->color_matrix, inverse, second);
				const uint8_t yy0 = clamp_byte((int)(first[0] + 0.5f));
				const uint8_t yy1 = clamp_byte((int)(second[0] + 0.5f));
				const uint8_t u = clamp_byte((int)((first[1] + second[1]) * 0.5f + 0.5f));
				const uint8_t v = clamp_byte((int)((first[2] + second[2]) * 0.5f + 0.5f));
				const size_t offset = (size_t)x * 2;
				if (frame->format == VIDEO_FORMAT_UYVY) {
					row[offset] = u; row[offset + 1] = yy0; row[offset + 2] = v; row[offset + 3] = yy1;
				} else {
					row[offset] = yy0; row[offset + 2] = yy1;
					row[offset + (frame->format == VIDEO_FORMAT_YVYU ? 3 : 1)] = u;
					row[offset + (frame->format == VIDEO_FORMAT_YVYU ? 1 : 3)] = v;
				}
			}
		} else {
			if (frame->format != VIDEO_FORMAT_I420 && frame->format != VIDEO_FORMAT_NV12)
				return false;
			if (frame->linesize[0] < frame->width)
				return false;
			for (uint32_t x = 0; x < frame->width; x++) {
				float yuv[3];
				rgb_to_yuv(rgb + ((size_t)y * frame->width + x) * 3, frame->color_matrix, inverse, yuv);
				frame->data[0][(size_t)dst_y * frame->linesize[0] + x] = clamp_byte((int)(yuv[0] + 0.5f));
			}
		}
	}

	if (packed)
		return true;
	const uint32_t cw = (frame->width + 1) / 2;
	const uint32_t ch = (frame->height + 1) / 2;
	if (!frame->data[1] || frame->linesize[1] < cw * (frame->format == VIDEO_FORMAT_NV12 ? 2 : 1) ||
	    (frame->format == VIDEO_FORMAT_I420 && (!frame->data[2] || frame->linesize[2] < cw)))
		return false;
	for (uint32_t cy = 0; cy < ch; cy++) {
		const uint32_t dst_cy = frame->flip ? ch - 1 - cy : cy;
		for (uint32_t cx = 0; cx < cw; cx++) {
			float sum_u = 0.0f, sum_v = 0.0f;
			uint32_t count = 0;
			for (uint32_t dy = 0; dy < 2 && cy * 2 + dy < frame->height; dy++) {
				for (uint32_t dx = 0; dx < 2 && cx * 2 + dx < frame->width; dx++) {
					float yuv[3];
					const size_t pixel = ((size_t)(cy * 2 + dy) * frame->width + cx * 2 + dx) * 3;
					rgb_to_yuv(rgb + pixel, frame->color_matrix, inverse, yuv);
					sum_u += yuv[1]; sum_v += yuv[2]; count++;
				}
			}
			const uint8_t u = clamp_byte((int)(sum_u / count + 0.5f));
			const uint8_t v = clamp_byte((int)(sum_v / count + 0.5f));
			if (frame->format == VIDEO_FORMAT_NV12) {
				uint8_t *uv = frame->data[1] + (size_t)dst_cy * frame->linesize[1] + cx * 2;
				uv[0] = u; uv[1] = v;
			} else {
				frame->data[1][(size_t)dst_cy * frame->linesize[1] + cx] = u;
				frame->data[2][(size_t)dst_cy * frame->linesize[2] + cx] = v;
			}
		}
	}
	return true;
}

bool face_swap_frame_pixel_to_rgb(uint8_t rgb[3], const struct obs_source_frame *frame, uint32_t x, uint32_t y)
{
	if (!rgb || !frame || !frame->data[0] || x >= frame->width || y >= frame->height)
		return false;

	const uint32_t src_y = frame->flip ? frame->height - 1 - y : y;
	int yy;
	int u;
	int v;

	switch (frame->format) {
	case VIDEO_FORMAT_I420:
		if (!frame->data[1] || !frame->data[2])
			return false;
		yy = sample_plane(frame->data[0], frame->linesize[0], x, src_y);
		u = sample_plane(frame->data[1], frame->linesize[1], x / 2, src_y / 2);
		v = sample_plane(frame->data[2], frame->linesize[2], x / 2, src_y / 2);
		break;
	case VIDEO_FORMAT_NV12:
		if (!frame->data[1])
			return false;
		yy = sample_plane(frame->data[0], frame->linesize[0], x, src_y);
		u = sample_plane(frame->data[1], frame->linesize[1], (x / 2) * 2, src_y / 2);
		v = sample_plane(frame->data[1], frame->linesize[1], (x / 2) * 2 + 1, src_y / 2);
		break;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY: {
		const uint8_t *row = frame->data[0] + (size_t)src_y * frame->linesize[0];
		const uint32_t pair = (x / 2) * 4;
		if (frame->format == VIDEO_FORMAT_UYVY) {
			u = row[pair];
			v = row[pair + 2];
			yy = row[pair + 1 + (x & 1) * 2];
		} else {
			yy = row[pair + (x & 1) * 2];
			u = row[pair + (frame->format == VIDEO_FORMAT_YVYU ? 3 : 1)];
			v = row[pair + (frame->format == VIDEO_FORMAT_YVYU ? 1 : 3)];
		}
		break;
	}
	default:
		return false;
	}

	yuv_to_rgb(rgb, yy, u, v, frame->color_matrix);
	return true;
}

bool face_swap_rgb_pixel_to_frame(struct obs_source_frame *frame, uint32_t x, uint32_t y, const uint8_t rgb[3])
{
	if (!frame || !rgb || !frame->data[0] || x >= frame->width || y >= frame->height)
		return false;

	float inverse[9];
	if (!invert_color_matrix(frame->color_matrix, inverse))
		return false;

	float yuv[3];
	rgb_to_yuv(rgb, frame->color_matrix, inverse, yuv);
	const uint32_t dst_y = frame->flip ? frame->height - 1 - y : y;
	const uint8_t luma = clamp_byte((int)(yuv[0] + 0.5f));
	const uint8_t chroma_u = clamp_byte((int)(yuv[1] + 0.5f));
	const uint8_t chroma_v = clamp_byte((int)(yuv[2] + 0.5f));

	switch (frame->format) {
	case VIDEO_FORMAT_I420:
		if (!frame->data[1] || !frame->data[2])
			return false;
		frame->data[0][(size_t)dst_y * frame->linesize[0] + x] = luma;
		frame->data[1][(size_t)(dst_y / 2) * frame->linesize[1] + x / 2] = chroma_u;
		frame->data[2][(size_t)(dst_y / 2) * frame->linesize[2] + x / 2] = chroma_v;
		return true;
	case VIDEO_FORMAT_NV12: {
		if (!frame->data[1])
			return false;
		frame->data[0][(size_t)dst_y * frame->linesize[0] + x] = luma;
		uint8_t *uv = frame->data[1] + (size_t)(dst_y / 2) * frame->linesize[1] + (x / 2) * 2;
		uv[0] = chroma_u;
		uv[1] = chroma_v;
		return true;
	}
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY: {
		uint8_t *row = frame->data[0] + (size_t)dst_y * frame->linesize[0];
		const uint32_t pair = (x / 2) * 4;
		if (frame->format == VIDEO_FORMAT_UYVY) {
			row[pair] = chroma_u;
			row[pair + 2] = chroma_v;
			row[pair + 1 + (x & 1) * 2] = luma;
		} else {
			row[pair + (x & 1) * 2] = luma;
			row[pair + (frame->format == VIDEO_FORMAT_YVYU ? 3 : 1)] = chroma_u;
			row[pair + (frame->format == VIDEO_FORMAT_YVYU ? 1 : 3)] = chroma_v;
		}
		return true;
	}
	default:
		return false;
	}
}

static inline float lerp(float a, float b, float amount)
{
	return a + (b - a) * amount;
}

bool face_swap_rgb_to_chw(float *chw, size_t chw_count, const uint8_t *rgb, uint32_t src_width,
			  uint32_t src_height, uint32_t dst_width, uint32_t dst_height,
			  const struct face_swap_tensor_params *params)
{
	if (!chw || !rgb || !params || !src_width || !src_height || !dst_width || !dst_height ||
	    dst_width > SIZE_MAX / 3 / dst_height || chw_count < (size_t)dst_width * dst_height * 3)
		return false;

	const size_t plane_size = (size_t)dst_width * dst_height;
	for (uint32_t y = 0; y < dst_height; y++) {
		const float src_y = ((float)y + 0.5f) * (float)src_height / (float)dst_height - 0.5f;
		const uint32_t y0 = src_y > 0.0f ? (uint32_t)src_y : 0;
		const uint32_t y1 = y0 + 1 < src_height ? y0 + 1 : y0;
		const float fy = src_y > 0.0f ? src_y - (float)y0 : 0.0f;

		for (uint32_t x = 0; x < dst_width; x++) {
			const float src_x = ((float)x + 0.5f) * (float)src_width / (float)dst_width - 0.5f;
			const uint32_t x0 = src_x > 0.0f ? (uint32_t)src_x : 0;
			const uint32_t x1 = x0 + 1 < src_width ? x0 + 1 : x0;
			const float fx = src_x > 0.0f ? src_x - (float)x0 : 0.0f;
			const size_t dst_index = (size_t)y * dst_width + x;

			for (size_t channel = 0; channel < 3; channel++) {
				const size_t source_channel = params->bgr ? 2 - channel : channel;
				const float top = lerp((float)rgb[((size_t)y0 * src_width + x0) * 3 + source_channel],
						       (float)rgb[((size_t)y0 * src_width + x1) * 3 + source_channel], fx);
				const float bottom = lerp((float)rgb[((size_t)y1 * src_width + x0) * 3 + source_channel],
							  (float)rgb[((size_t)y1 * src_width + x1) * 3 + source_channel], fx);
				const float value = lerp(top, bottom, fy);
				chw[channel * plane_size + dst_index] =
					(value - params->mean[channel]) * params->scale[channel];
			}
		}
	}

	return true;
}

bool face_swap_rgb_to_letterbox_chw(float *chw, size_t chw_count, const uint8_t *rgb, uint32_t src_width,
					uint32_t src_height, uint32_t dst_width, uint32_t dst_height,
					const struct face_swap_tensor_params *params, struct face_swap_letterbox *letterbox)
{
	if (!letterbox || !src_width || !src_height || !dst_width || !dst_height)
		return false;

	const float scale = fminf((float)dst_width / src_width, (float)dst_height / src_height);
	const uint32_t resized_width = (uint32_t)fmaxf(1.0f, floorf(src_width * scale + 0.5f));
	const uint32_t resized_height = (uint32_t)fmaxf(1.0f, floorf(src_height * scale + 0.5f));
	const float pad_x = ((float)dst_width - resized_width) * 0.5f;
	const float pad_y = ((float)dst_height - resized_height) * 0.5f;

	if (!chw || !rgb || !params || dst_width > SIZE_MAX / 3 / dst_height ||
	    chw_count < (size_t)dst_width * dst_height * 3)
		return false;

	const size_t plane_size = (size_t)dst_width * dst_height;
	for (uint32_t y = 0; y < dst_height; y++) {
		for (uint32_t x = 0; x < dst_width; x++) {
			const float source_x = ((float)x - pad_x + 0.5f) / scale - 0.5f;
			const float source_y = ((float)y - pad_y + 0.5f) / scale - 0.5f;
			const bool inside = source_x >= 0.0f && source_y >= 0.0f && source_x < src_width && source_y < src_height;
			const uint32_t x0 = inside ? (uint32_t)source_x : 0;
			const uint32_t y0 = inside ? (uint32_t)source_y : 0;
			const uint32_t x1 = inside && x0 + 1 < src_width ? x0 + 1 : x0;
			const uint32_t y1 = inside && y0 + 1 < src_height ? y0 + 1 : y0;
			const float fx = inside ? source_x - x0 : 0.0f;
			const float fy = inside ? source_y - y0 : 0.0f;
			const size_t dst_index = (size_t)y * dst_width + x;

			for (size_t channel = 0; channel < 3; channel++) {
				const size_t source_channel = params->bgr ? 2 - channel : channel;
				float value = 114.0f;
				if (inside) {
					const float top = lerp((float)rgb[((size_t)y0 * src_width + x0) * 3 + source_channel],
							       (float)rgb[((size_t)y0 * src_width + x1) * 3 + source_channel], fx);
					const float bottom = lerp((float)rgb[((size_t)y1 * src_width + x0) * 3 + source_channel],
								  (float)rgb[((size_t)y1 * src_width + x1) * 3 + source_channel], fx);
					value = lerp(top, bottom, fy);
				}
				chw[channel * plane_size + dst_index] = (value - params->mean[channel]) * params->scale[channel];
			}
		}
	}

	letterbox->scale = scale;
	letterbox->pad_x = pad_x;
	letterbox->pad_y = pad_y;
	letterbox->source_width = src_width;
	letterbox->source_height = src_height;
	letterbox->target_width = dst_width;
	letterbox->target_height = dst_height;
	return true;
}
