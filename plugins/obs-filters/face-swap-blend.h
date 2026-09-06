#pragma once

#include "face-swap-align.h"

#include <obs.h>

#include <stdbool.h>
#include <stdint.h>

bool face_swap_blend_crop_rgb(uint8_t *rgb, uint32_t width, uint32_t height, const uint8_t *crop,
			      const struct face_swap_affine *forward, float strength, float opacity);

/* Blends the swapped crop directly into a live frame.  Only pixels inside the
 * face mask are touched, so the background always comes from this frame rather
 * than from the (older) frame the inference ran on.  Allocation-free, so it is
 * safe to call from the video thread. */
bool face_swap_blend_crop_frame(struct obs_source_frame *frame, const uint8_t *crop,
				const struct face_swap_affine *forward, float strength, float opacity);
