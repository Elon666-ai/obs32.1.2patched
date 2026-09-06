#pragma once

#include "face-swap-scrfd.h"

struct face_swap_stabilizer {
	struct face_swap_detection value;
	bool valid;
	uint32_t stable_frames;
	uint32_t lost_frames;
	float opacity;
};

void face_swap_stabilizer_reset(struct face_swap_stabilizer *stabilizer);
void face_swap_stabilizer_update(struct face_swap_stabilizer *stabilizer,
				 const struct face_swap_detection *detection);
