#pragma once

#include <stdbool.h>
#include <stdint.h>

struct face_swap_image {
	uint8_t *rgb;
	uint32_t width;
	uint32_t height;
};

bool face_swap_image_load(struct face_swap_image *image, const char *path);
void face_swap_image_free(struct face_swap_image *image);
