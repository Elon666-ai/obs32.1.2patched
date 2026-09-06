#pragma once

#include "face-swap-ort.h"
#include "face-swap-preprocess.h"

struct face_swap_detection {
	float x1;
	float y1;
	float x2;
	float y2;
	float score;
	float landmarks[10];
};

bool face_swap_scrfd_decode(void *param, const struct face_swap_ort_tensor *outputs, size_t output_count);
bool face_swap_detection_unletterbox(struct face_swap_detection *detection,
				     const struct face_swap_letterbox *letterbox);
