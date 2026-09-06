#include "face-swap-align.h"

#include <math.h>

static const float template_112[10] = {
	38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f,
	71.7366f, 41.5493f, 92.3655f, 70.7299f, 92.2041f,
};

bool face_swap_estimate_alignment(const struct face_swap_detection *detection, struct face_swap_affine *forward)
{
	if (!detection || !forward || detection->score <= 0.0f)
		return false;

	float source_mean_x = 0.0f;
	float source_mean_y = 0.0f;
	float target_mean_x = 0.0f;
	float target_mean_y = 0.0f;
	const float template_scale = (float)FACE_SWAP_CROP_SIZE / 112.0f;
	for (size_t i = 0; i < 5; i++) {
		const float x = detection->landmarks[i * 2];
		const float y = detection->landmarks[i * 2 + 1];
		if (!isfinite(x) || !isfinite(y))
			return false;
		source_mean_x += x;
		source_mean_y += y;
		target_mean_x += template_112[i * 2] * template_scale;
		target_mean_y += template_112[i * 2 + 1] * template_scale;
	}
	source_mean_x /= 5.0f;
	source_mean_y /= 5.0f;
	target_mean_x /= 5.0f;
	target_mean_y /= 5.0f;

	float denominator = 0.0f;
	float real = 0.0f;
	float imaginary = 0.0f;
	for (size_t i = 0; i < 5; i++) {
		const float sx = detection->landmarks[i * 2] - source_mean_x;
		const float sy = detection->landmarks[i * 2 + 1] - source_mean_y;
		const float tx = template_112[i * 2] * template_scale - target_mean_x;
		const float ty = template_112[i * 2 + 1] * template_scale - target_mean_y;
		denominator += sx * sx + sy * sy;
		real += sx * tx + sy * ty;
		imaginary += sx * ty - sy * tx;
	}
	if (!isfinite(denominator) || denominator < 1e-4f)
		return false;

	forward->a = real / denominator;
	forward->b = imaginary / denominator;
	forward->tx = target_mean_x - forward->a * source_mean_x + forward->b * source_mean_y;
	forward->ty = target_mean_y - forward->b * source_mean_x - forward->a * source_mean_y;
	return isfinite(forward->a) && isfinite(forward->b) &&
	       forward->a * forward->a + forward->b * forward->b > 1e-8f;
}

static uint8_t sample_channel(const uint8_t *rgb, uint32_t width, uint32_t height, float x, float y, size_t channel)
{
	if (x < 0.0f || y < 0.0f || x > width - 1.0f || y > height - 1.0f)
		return 0;
	const uint32_t x0 = (uint32_t)x;
	const uint32_t y0 = (uint32_t)y;
	const uint32_t x1 = x0 + 1 < width ? x0 + 1 : x0;
	const uint32_t y1 = y0 + 1 < height ? y0 + 1 : y0;
	const float fx = x - x0;
	const float fy = y - y0;
	const float top = rgb[((size_t)y0 * width + x0) * 3 + channel] * (1.0f - fx) +
			  rgb[((size_t)y0 * width + x1) * 3 + channel] * fx;
	const float bottom = rgb[((size_t)y1 * width + x0) * 3 + channel] * (1.0f - fx) +
			     rgb[((size_t)y1 * width + x1) * 3 + channel] * fx;
	return (uint8_t)(top * (1.0f - fy) + bottom * fy + 0.5f);
}

bool face_swap_align_rgb(uint8_t *crop, const uint8_t *rgb, uint32_t width, uint32_t height,
			 const struct face_swap_affine *forward)
{
	if (!crop || !rgb || !width || !height || !forward)
		return false;
	const float determinant = forward->a * forward->a + forward->b * forward->b;
	if (!isfinite(determinant) || determinant < 1e-8f)
		return false;

	for (uint32_t y = 0; y < FACE_SWAP_CROP_SIZE; y++) {
		for (uint32_t x = 0; x < FACE_SWAP_CROP_SIZE; x++) {
			const float dx = x - forward->tx;
			const float dy = y - forward->ty;
			const float source_x = (forward->a * dx + forward->b * dy) / determinant;
			const float source_y = (-forward->b * dx + forward->a * dy) / determinant;
			for (size_t channel = 0; channel < 3; channel++)
				crop[((size_t)y * FACE_SWAP_CROP_SIZE + x) * 3 + channel] =
					sample_channel(rgb, width, height, source_x, source_y, channel);
		}
	}
	return true;
}
