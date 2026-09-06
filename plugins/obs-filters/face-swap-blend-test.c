#include "face-swap-blend.h"

#include <assert.h>
#include <string.h>

int main(void)
{
	uint8_t image[16 * 16 * 3];
	uint8_t crop[128 * 128 * 3];
	memset(image, 0, sizeof(image));
	memset(crop, 255, sizeof(crop));
	const struct face_swap_affine alignment = {.a = 8.0f, .b = 0.0f, .tx = -0.5f, .ty = -0.5f};
	assert(face_swap_blend_crop_rgb(image, 16, 16, crop, &alignment, 1.0f, 1.0f));
	assert(image[8 * 16 * 3 + 8 * 3] > 200);
	assert(image[0] == 0);

	uint8_t original[sizeof(image)];
	memcpy(original, image, sizeof(image));
	assert(face_swap_blend_crop_rgb(image, 16, 16, crop, &alignment, 0.0f, 1.0f));
	assert(memcmp(image, original, sizeof(image)) == 0);
	return 0;
}
