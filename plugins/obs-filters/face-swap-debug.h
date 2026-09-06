#pragma once

#include "face-swap-scrfd.h"

#include <obs.h>

bool face_swap_draw_detection(struct obs_source_frame *frame, const struct face_swap_detection *detection,
			      float opacity);
