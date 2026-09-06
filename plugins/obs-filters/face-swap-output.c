#include "face-swap-output.h"

#include "face-swap-align.h"

#include <math.h>

bool face_swap_capture_swap(void *param, const struct face_swap_ort_tensor *outputs, size_t output_count)
{
	uint8_t *crop = param;
	if (!crop || !outputs || output_count != 1 || !outputs[0].data || outputs[0].rank != 4 ||
	    outputs[0].shape[0] != 1 || outputs[0].shape[1] != 3 || outputs[0].shape[2] != FACE_SWAP_CROP_SIZE ||
	    outputs[0].shape[3] != FACE_SWAP_CROP_SIZE ||
	    outputs[0].element_count != 3 * FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE)
		return false;

	/* Image-to-image generators in this pipeline emit [0, 1] RGB.  A small
	 * tolerance absorbs the overshoot typical of tanh/sigmoid heads. */
	const size_t plane_size = FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE;
	for (size_t i = 0; i < plane_size; i++) {
		for (size_t channel = 0; channel < 3; channel++) {
			const float value = outputs[0].data[channel * plane_size + i];
			if (!isfinite(value) || value < -0.05f || value > 1.05f)
				return false;
			crop[i * 3 + channel] = (uint8_t)fminf(255.0f, fmaxf(0.0f, value * 255.0f + 0.5f));
		}
	}
	return true;
}
