#include "face-swap-yunet.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_SIZE 640

static void set_tensor(struct face_swap_ort_tensor *tensor, float *data, size_t anchors, size_t columns)
{
	tensor->data = data;
	tensor->element_count = anchors * columns;
	tensor->rank = 3;
	tensor->shape[0] = 1;
	tensor->shape[1] = (int64_t)anchors;
	tensor->shape[2] = (int64_t)columns;
}

int main(void)
{
	const uint32_t strides[3] = {8, 16, 32};
	struct face_swap_ort_tensor outputs[12] = {0};
	float *buffers[12] = {0};
	size_t anchors[3];

	/* Model output order: cls x3, obj x3, bbox x3, kps x3. */
	for (size_t g = 0; g < 3; g++) {
		const size_t side = INPUT_SIZE / strides[g];
		anchors[g] = side * side;
		buffers[g] = calloc(anchors[g], sizeof(float));
		buffers[g + 3] = calloc(anchors[g], sizeof(float));
		buffers[g + 6] = calloc(anchors[g] * 4, sizeof(float));
		buffers[g + 9] = calloc(anchors[g] * 10, sizeof(float));
		assert(buffers[g] && buffers[g + 3] && buffers[g + 6] && buffers[g + 9]);
		set_tensor(&outputs[g], buffers[g], anchors[g], 1);
		set_tensor(&outputs[g + 3], buffers[g + 3], anchors[g], 1);
		set_tensor(&outputs[g + 6], buffers[g + 6], anchors[g], 4);
		set_tensor(&outputs[g + 9], buffers[g + 9], anchors[g], 10);
	}

	struct face_swap_detection detection;

	/* No confidence anywhere: valid layout, no face. */
	assert(face_swap_yunet_decode(&detection, outputs, 12));
	assert(detection.score == 0.0f);

	/* Plant a face on the stride-16 grid at cell (10, 5). */
	const uint32_t side16 = INPUT_SIZE / 16;
	const size_t cell = 5 * side16 + 10;
	buffers[1][cell] = 0.81f; /* cls */
	buffers[4][cell] = 0.81f; /* obj -> sqrt(0.81 * 0.81) = 0.81 */

	float *bbox = buffers[7] + cell * 4;
	bbox[0] = 0.5f;        /* x offset in stride units */
	bbox[1] = 0.25f;       /* y offset */
	bbox[2] = logf(4.0f);  /* width  = e^ln4 * 16 = 64 */
	bbox[3] = logf(5.0f);  /* height = e^ln5 * 16 = 80 */

	float *kps = buffers[10] + cell * 10;
	for (size_t n = 0; n < 5; n++) {
		kps[n * 2] = 1.0f + (float)n;
		kps[n * 2 + 1] = 2.0f;
	}

	assert(face_swap_yunet_decode(&detection, outputs, 12));
	assert(fabsf(detection.score - 0.81f) < 0.001f);

	/* x = (10 + 0.5) * 16 = 168, y = (5 + 0.25) * 16 = 84 */
	assert(fabsf(detection.x1 - 168.0f) < 0.01f);
	assert(fabsf(detection.y1 - 84.0f) < 0.01f);
	assert(fabsf(detection.x2 - 232.0f) < 0.01f);
	assert(fabsf(detection.y2 - 164.0f) < 0.01f);

	/* landmark 0 = ((10 + 1) * 16, (5 + 2) * 16) = (176, 112) */
	assert(fabsf(detection.landmarks[0] - 176.0f) < 0.01f);
	assert(fabsf(detection.landmarks[1] - 112.0f) < 0.01f);

	/* Below the 0.6 threshold must not register. */
	memset(buffers[1], 0, anchors[1] * sizeof(float));
	memset(buffers[4], 0, anchors[1] * sizeof(float));
	buffers[1][cell] = 0.3f;
	buffers[4][cell] = 0.3f;
	assert(face_swap_yunet_decode(&detection, outputs, 12));
	assert(detection.score == 0.0f);

	/* SCRFD's 9-output layout must be rejected outright. */
	assert(!face_swap_yunet_decode(&detection, outputs, 9));

	for (size_t i = 0; i < 12; i++)
		free(buffers[i]);
	return 0;
}
