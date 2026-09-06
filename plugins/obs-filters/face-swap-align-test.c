#include "face-swap-align.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static const float template_128[10] = {
	43.7653f, 59.0815f, 84.0363f, 58.8587f, 64.0288f,
	81.9847f, 47.4850f, 105.5606f, 80.8342f, 105.3761f,
};

int main(void)
{
	struct face_swap_detection detection = {.score = 0.9f};
	for (size_t i = 0; i < 10; i++)
		detection.landmarks[i] = template_128[i];
	struct face_swap_affine affine;
	assert(face_swap_estimate_alignment(&detection, &affine));
	assert(fabsf(affine.a - 1.0f) < 0.001f);
	assert(fabsf(affine.b) < 0.001f);
	assert(fabsf(affine.tx) < 0.001f && fabsf(affine.ty) < 0.001f);

	uint8_t image[FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE * 3];
	uint8_t crop[sizeof(image)];
	for (size_t i = 0; i < sizeof(image); i++)
		image[i] = (uint8_t)(i % 251);
	const struct face_swap_affine identity = {.a = 1.0f, .b = 0.0f, .tx = 0.0f, .ty = 0.0f};
	assert(face_swap_align_rgb(crop, image, FACE_SWAP_CROP_SIZE, FACE_SWAP_CROP_SIZE, &identity));
	assert(memcmp(crop, image, sizeof(image)) == 0);

	for (size_t i = 0; i < 10; i++)
		detection.landmarks[i] = 10.0f;
	assert(!face_swap_estimate_alignment(&detection, &affine));
	return 0;
}
