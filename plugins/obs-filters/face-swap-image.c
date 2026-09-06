#include "face-swap-image.h"

#include <graphics/graphics.h>
#include <util/bmem.h>

#include <string.h>

#define FACE_SWAP_MAX_SOURCE_PIXELS (8192U * 8192U)

bool face_swap_image_load(struct face_swap_image *image, const char *path)
{
	if (!image || !path || !*path)
		return false;

	face_swap_image_free(image);
	enum gs_color_format format = GS_UNKNOWN;
	uint32_t width = 0;
	uint32_t height = 0;
	uint8_t *pixels = gs_create_texture_file_data2(path, GS_IMAGE_ALPHA_STRAIGHT, &format, &width, &height);
	if (!pixels || !width || !height || width > FACE_SWAP_MAX_SOURCE_PIXELS / height ||
	    (format != GS_RGBA && format != GS_BGRA)) {
		bfree(pixels);
		return false;
	}

	const size_t pixel_count = (size_t)width * height;
	uint8_t *rgb = bmalloc(pixel_count * 3);
	if (!rgb) {
		bfree(pixels);
		return false;
	}
	for (size_t i = 0; i < pixel_count; i++) {
		const size_t red = format == GS_RGBA ? 0 : 2;
		const size_t blue = format == GS_RGBA ? 2 : 0;
		rgb[i * 3] = pixels[i * 4 + red];
		rgb[i * 3 + 1] = pixels[i * 4 + 1];
		rgb[i * 3 + 2] = pixels[i * 4 + blue];
	}
	bfree(pixels);

	image->rgb = rgb;
	image->width = width;
	image->height = height;
	return true;
}

void face_swap_image_free(struct face_swap_image *image)
{
	if (!image)
		return;
	bfree(image->rgb);
	memset(image, 0, sizeof(*image));
}
