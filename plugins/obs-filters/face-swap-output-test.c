#include "face-swap-output.h"

#include "face-swap-align.h"

#include <assert.h>
#include <stdlib.h>

int main(void)
{
	const size_t plane = FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE;
	float *data = calloc(plane * 3, sizeof(*data));
	uint8_t *crop = malloc(plane * 3);
	assert(data && crop);

	/* Generator output is [0, 1]; 0 -> 0, 0.5 -> 128, 1 -> 255. */
	data[0] = 0.0f;
	data[plane] = 0.5f;
	data[plane * 2] = 1.0f;
	struct face_swap_ort_tensor output = {
		.data = data,
		.shape = {1, 3, FACE_SWAP_CROP_SIZE, FACE_SWAP_CROP_SIZE},
		.rank = 4,
		.element_count = plane * 3,
	};
	assert(face_swap_capture_swap(crop, &output, 1));
	assert(crop[0] == 0 && crop[1] == 128 && crop[2] == 255);

	/* Values well outside [0, 1] indicate a mismatched model contract. */
	data[0] = 2.0f;
	assert(!face_swap_capture_swap(crop, &output, 1));
	data[0] = -1.0f;
	assert(!face_swap_capture_swap(crop, &output, 1));

	free(crop);
	free(data);
	return 0;
}
