#pragma once

#include <obs.h>

#include <stddef.h>
#include <stdint.h>

/* Produces interleaved RGB bytes.  The caller owns rgb and must provide
 * width * height * 3 bytes. */
bool face_swap_frame_to_rgb(uint8_t *rgb, size_t rgb_size, const struct obs_source_frame *frame);
bool face_swap_rgb_to_frame(struct obs_source_frame *frame, const uint8_t *rgb, size_t rgb_size);

/* Single-pixel access for in-place blending on the video thread. */
bool face_swap_frame_pixel_to_rgb(uint8_t rgb[3], const struct obs_source_frame *frame, uint32_t x, uint32_t y);
bool face_swap_rgb_pixel_to_frame(struct obs_source_frame *frame, uint32_t x, uint32_t y, const uint8_t rgb[3]);

struct face_swap_tensor_params {
	float mean[3];
	float scale[3];
	bool bgr;
};

struct face_swap_letterbox {
	float scale;
	float pad_x;
	float pad_y;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t target_width;
	uint32_t target_height;
};

/* Resizes interleaved RGB bytes and writes planar CHW floats. */
bool face_swap_rgb_to_chw(float *chw, size_t chw_count, const uint8_t *rgb, uint32_t src_width,
			  uint32_t src_height, uint32_t dst_width, uint32_t dst_height,
			  const struct face_swap_tensor_params *params);

bool face_swap_rgb_to_letterbox_chw(float *chw, size_t chw_count, const uint8_t *rgb, uint32_t src_width,
					uint32_t src_height, uint32_t dst_width, uint32_t dst_height,
					const struct face_swap_tensor_params *params, struct face_swap_letterbox *letterbox);
