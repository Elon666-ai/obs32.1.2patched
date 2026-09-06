#pragma once

#include "face-swap-scrfd.h"

#include <stdbool.h>
#include <stdint.h>

#define FACE_SWAP_CROP_SIZE 128

struct face_swap_affine {
	float a;
	float b;
	float tx;
	float ty;
};

bool face_swap_estimate_alignment(const struct face_swap_detection *detection, struct face_swap_affine *forward);
bool face_swap_align_rgb(uint8_t *crop, const uint8_t *rgb, uint32_t width, uint32_t height,
			 const struct face_swap_affine *forward);
