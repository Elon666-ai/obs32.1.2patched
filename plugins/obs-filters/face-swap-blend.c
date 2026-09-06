#include "face-swap-blend.h"

#include "face-swap-preprocess.h"

#include <float.h>
#include <math.h>
#include <string.h>

static float smoothstep(float edge0, float edge1, float value)
{
	float t = (value - edge0) / (edge1 - edge0);
	t = fminf(1.0f, fmaxf(0.0f, t));
	return t * t * (3.0f - 2.0f * t);
}

static float sample_crop(const uint8_t *crop, float x, float y, size_t channel)
{
	const uint32_t x0 = (uint32_t)x;
	const uint32_t y0 = (uint32_t)y;
	const uint32_t x1 = x0 + 1 < FACE_SWAP_CROP_SIZE ? x0 + 1 : x0;
	const uint32_t y1 = y0 + 1 < FACE_SWAP_CROP_SIZE ? y0 + 1 : y0;
	const float fx = x - x0;
	const float fy = y - y0;
	const float top = crop[((size_t)y0 * FACE_SWAP_CROP_SIZE + x0) * 3 + channel] * (1.0f - fx) +
			  crop[((size_t)y0 * FACE_SWAP_CROP_SIZE + x1) * 3 + channel] * fx;
	const float bottom = crop[((size_t)y1 * FACE_SWAP_CROP_SIZE + x0) * 3 + channel] * (1.0f - fx) +
			     crop[((size_t)y1 * FACE_SWAP_CROP_SIZE + x1) * 3 + channel] * fx;
	return top * (1.0f - fy) + bottom * fy;
}

bool face_swap_blend_crop_rgb(uint8_t *rgb, uint32_t width, uint32_t height, const uint8_t *crop,
			      const struct face_swap_affine *forward, float strength, float opacity)
{
	if (!rgb || !crop || !forward || !width || !height || !isfinite(strength) || !isfinite(opacity))
		return false;
	const float determinant = forward->a * forward->a + forward->b * forward->b;
	if (!isfinite(determinant) || determinant < 1e-8f)
		return false;
	const float amount = fminf(1.0f, fmaxf(0.0f, strength)) * fminf(1.0f, fmaxf(0.0f, opacity));
	if (amount <= 0.0f)
		return true;

	/* Conservative full-frame loop for correctness.  The worker is limited
	 * to inference FPS; a transformed bounding box optimization can follow
	 * after profiling. */
	const float center = (FACE_SWAP_CROP_SIZE - 1.0f) * 0.5f;
	const float radius_x = FACE_SWAP_CROP_SIZE * 0.40f;
	const float radius_y = FACE_SWAP_CROP_SIZE * 0.47f;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			const float crop_x = forward->a * x - forward->b * y + forward->tx;
			const float crop_y = forward->b * x + forward->a * y + forward->ty;
			if (crop_x < 0.0f || crop_y < 0.0f || crop_x > FACE_SWAP_CROP_SIZE - 1.0f ||
			    crop_y > FACE_SWAP_CROP_SIZE - 1.0f)
				continue;
			const float nx = (crop_x - center) / radius_x;
			const float ny = (crop_y - center) / radius_y;
			const float distance = sqrtf(nx * nx + ny * ny);
			const float alpha = amount * (1.0f - smoothstep(0.78f, 1.0f, distance));
			if (alpha <= 0.0f)
				continue;
			uint8_t *pixel = rgb + ((size_t)y * width + x) * 3;
			for (size_t channel = 0; channel < 3; channel++) {
				const float swapped = sample_crop(crop, crop_x, crop_y, channel);
				pixel[channel] = (uint8_t)(pixel[channel] * (1.0f - alpha) + swapped * alpha + 0.5f);
			}
		}
	}
	return true;
}

bool face_swap_blend_crop_frame(struct obs_source_frame *frame, const uint8_t *crop,
				const struct face_swap_affine *forward, float strength, float opacity)
{
	if (!frame || !crop || !forward || !isfinite(strength) || !isfinite(opacity))
		return false;
	const float determinant = forward->a * forward->a + forward->b * forward->b;
	if (!isfinite(determinant) || determinant < 1e-8f)
		return false;
	const float amount = fminf(1.0f, fmaxf(0.0f, strength)) * fminf(1.0f, fmaxf(0.0f, opacity));
	if (amount <= 0.0f)
		return true;

	/* Map the crop corners back to frame space so only the face
	 * neighbourhood is visited; the rest of the frame is untouched. */
	float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
	const float corners[4][2] = {
		{0.0f, 0.0f},
		{FACE_SWAP_CROP_SIZE - 1.0f, 0.0f},
		{0.0f, FACE_SWAP_CROP_SIZE - 1.0f},
		{FACE_SWAP_CROP_SIZE - 1.0f, FACE_SWAP_CROP_SIZE - 1.0f},
	};
	for (size_t i = 0; i < 4; i++) {
		const float dx = corners[i][0] - forward->tx;
		const float dy = corners[i][1] - forward->ty;
		const float x = (forward->a * dx + forward->b * dy) / determinant;
		const float y = (-forward->b * dx + forward->a * dy) / determinant;
		min_x = fminf(min_x, x);
		min_y = fminf(min_y, y);
		max_x = fmaxf(max_x, x);
		max_y = fmaxf(max_y, y);
	}
	if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(max_x) || !isfinite(max_y))
		return false;

	const uint32_t x_begin = min_x > 0.0f ? (uint32_t)min_x : 0;
	const uint32_t y_begin = min_y > 0.0f ? (uint32_t)min_y : 0;
	const uint32_t x_end = max_x < frame->width - 1.0f ? (uint32_t)max_x + 1 : frame->width - 1;
	const uint32_t y_end = max_y < frame->height - 1.0f ? (uint32_t)max_y + 1 : frame->height - 1;
	if (x_begin > x_end || y_begin > y_end)
		return true;

	const float center = (FACE_SWAP_CROP_SIZE - 1.0f) * 0.5f;
	const float radius_x = FACE_SWAP_CROP_SIZE * 0.40f;
	const float radius_y = FACE_SWAP_CROP_SIZE * 0.47f;
	for (uint32_t y = y_begin; y <= y_end; y++) {
		for (uint32_t x = x_begin; x <= x_end; x++) {
			const float crop_x = forward->a * x - forward->b * y + forward->tx;
			const float crop_y = forward->b * x + forward->a * y + forward->ty;
			if (crop_x < 0.0f || crop_y < 0.0f || crop_x > FACE_SWAP_CROP_SIZE - 1.0f ||
			    crop_y > FACE_SWAP_CROP_SIZE - 1.0f)
				continue;
			const float nx = (crop_x - center) / radius_x;
			const float ny = (crop_y - center) / radius_y;
			const float alpha = amount * (1.0f - smoothstep(0.78f, 1.0f, sqrtf(nx * nx + ny * ny)));
			if (alpha <= 0.0f)
				continue;

			uint8_t rgb[3];
			if (!face_swap_frame_pixel_to_rgb(rgb, frame, x, y))
				return false;
			for (size_t channel = 0; channel < 3; channel++) {
				const float swapped = sample_crop(crop, crop_x, crop_y, channel);
				rgb[channel] = (uint8_t)(rgb[channel] * (1.0f - alpha) + swapped * alpha + 0.5f);
			}
			if (!face_swap_rgb_pixel_to_frame(frame, x, y, rgb))
				return false;
		}
	}
	return true;
}
