#include "face-swap-scrfd.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void set_tensor(struct face_swap_ort_tensor *tensor, float *data, size_t anchors, size_t columns)
{
	tensor->data = data;
	tensor->element_count = anchors * columns;
	tensor->rank = 2;
	tensor->shape[0] = (int64_t)anchors;
	tensor->shape[1] = (int64_t)columns;
}

int main(void)
{
	const size_t anchors[] = {12800, 3200, 800};
	struct face_swap_ort_tensor outputs[9] = {0};
	float *buffers[9] = {0};

	for (size_t group = 0; group < 3; group++) {
		buffers[group * 3] = calloc(anchors[group], sizeof(float));
		buffers[group * 3 + 1] = calloc(anchors[group] * 4, sizeof(float));
		buffers[group * 3 + 2] = calloc(anchors[group] * 10, sizeof(float));
		assert(buffers[group * 3] && buffers[group * 3 + 1] && buffers[group * 3 + 2]);
		set_tensor(&outputs[group * 3], buffers[group * 3], anchors[group], 1);
		set_tensor(&outputs[group * 3 + 1], buffers[group * 3 + 1], anchors[group], 4);
		set_tensor(&outputs[group * 3 + 2], buffers[group * 3 + 2], anchors[group], 10);
	}

	const size_t candidate = (5 * 40 + 4) * 2;
	buffers[3][candidate] = 0.9f;
	float *bbox = buffers[4] + candidate * 4;
	bbox[0] = 1.0f;
	bbox[1] = 2.0f;
	bbox[2] = 3.0f;
	bbox[3] = 4.0f;
	float *landmarks = buffers[5] + candidate * 10;
	for (size_t i = 0; i < 5; i++) {
		landmarks[i * 2] = (float)i;
		landmarks[i * 2 + 1] = (float)i + 0.5f;
	}

	struct face_swap_detection detection;
	assert(face_swap_scrfd_decode(&detection, outputs, 9));
	assert(detection.score == 0.9f);
	assert(detection.x1 == 48.0f && detection.y1 == 48.0f);
	assert(detection.x2 == 112.0f && detection.y2 == 144.0f);
	assert(detection.landmarks[0] == 64.0f && detection.landmarks[1] == 88.0f);
	assert(!face_swap_scrfd_decode(&detection, outputs, 8));
	struct face_swap_letterbox letterbox = {
		.scale = 0.5f,
		.pad_x = 0.0f,
		.pad_y = 160.0f,
		.source_width = 1280,
		.source_height = 720,
	};
	detection.x1 = 100.0f;
	detection.y1 = 180.0f;
	detection.x2 = 300.0f;
	detection.y2 = 280.0f;
	detection.score = 0.9f;
	for (size_t i = 0; i < 5; i++) {
		detection.landmarks[i * 2] = 100.0f;
		detection.landmarks[i * 2 + 1] = 180.0f;
	}
	assert(face_swap_detection_unletterbox(&detection, &letterbox));
	assert(detection.x1 == 200.0f && detection.y1 == 40.0f);
	assert(detection.x2 == 600.0f && detection.y2 == 240.0f);

	for (size_t i = 0; i < 9; i++)
		free(buffers[i]);
	return 0;
}
