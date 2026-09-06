#include "face-swap-debug.h"

#include <assert.h>
#include <string.h>

int main(void)
{
	uint8_t pixels[8 * 8];
	memset(pixels, 16, sizeof(pixels));
	struct obs_source_frame frame = {
		.data = {[0] = pixels},
		.linesize = {[0] = 8},
		.width = 8,
		.height = 8,
		.format = VIDEO_FORMAT_I420,
	};
	struct face_swap_detection detection = {
		.x1 = 2.0f,
		.y1 = 2.0f,
		.x2 = 5.0f,
		.y2 = 5.0f,
		.score = 0.9f,
		.landmarks = {3.0f, 3.0f, 4.0f, 3.0f, 3.0f, 4.0f, 3.0f, 4.0f, 4.0f, 4.0f},
	};
	assert(face_swap_draw_detection(&frame, &detection, 1.0f));
	assert(pixels[2 * 8 + 2] == 255);
	assert(pixels[5 * 8 + 5] == 255);
	assert(pixels[3 * 8 + 3] == 255);
	assert(pixels[0] == 16);
	return 0;
}
