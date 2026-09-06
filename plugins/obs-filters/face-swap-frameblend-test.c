#include "face-swap-blend.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* BT.709 limited-range matrix as libobs hands it to filters. */
static void set_matrix(struct obs_source_frame *frame)
{
	static const float matrix[16] = {
		1.164384f, 0.000000f, 1.792741f, -0.972945f,
		1.164384f, -0.213249f, -0.532909f, 0.301483f,
		1.164384f, 2.112402f, 0.000000f, -1.133402f,
		0.0f, 0.0f, 0.0f, 1.0f,
	};
	memcpy(frame->color_matrix, matrix, sizeof(matrix));
}

static bool run_format(enum video_format format)
{
	const uint32_t width = 64;
	const uint32_t height = 64;
	struct obs_source_frame *frame = obs_source_frame_create(format, width, height);
	assert(frame);
	set_matrix(frame);

	const bool subsampled = format == VIDEO_FORMAT_I420 || format == VIDEO_FORMAT_NV12;
	for (size_t plane = 0; plane < MAX_AV_PLANES; plane++) {
		if (!frame->data[plane] || !frame->linesize[plane])
			continue;
		const size_t rows = (plane > 0 && subsampled) ? (height + 1) / 2 : height;
		memset(frame->data[plane], 128, (size_t)frame->linesize[plane] * rows);
	}

	uint8_t *before = malloc((size_t)frame->linesize[0] * height);
	assert(before);
	memcpy(before, frame->data[0], (size_t)frame->linesize[0] * height);

	uint8_t crop[FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE * 3];
	memset(crop, 255, sizeof(crop));

	/* Maps the crop onto a 32x32 region centred in the frame. */
	const struct face_swap_affine affine = {.a = 4.0f, .b = 0.0f, .tx = -64.0f, .ty = -64.0f};
	assert(face_swap_blend_crop_frame(frame, crop, &affine, 1.0f, 1.0f));

	const uint8_t center = frame->data[0][(size_t)(height / 2) * frame->linesize[0] +
					      (format == VIDEO_FORMAT_I420 || format == VIDEO_FORMAT_NV12
						       ? width / 2
						       : (width / 2) * 2 + (format == VIDEO_FORMAT_UYVY ? 1 : 0))];
	assert(center > 128);
	assert(frame->data[0][0] == before[0]);

	/* Zero strength must leave every byte untouched. */
	memcpy(frame->data[0], before, (size_t)frame->linesize[0] * height);
	assert(face_swap_blend_crop_frame(frame, crop, &affine, 0.0f, 1.0f));
	assert(memcmp(frame->data[0], before, (size_t)frame->linesize[0] * height) == 0);

	free(before);
	obs_source_frame_destroy(frame);
	return true;
}

int main(void)
{
	assert(run_format(VIDEO_FORMAT_I420));
	assert(run_format(VIDEO_FORMAT_NV12));
	assert(run_format(VIDEO_FORMAT_YUY2));
	assert(run_format(VIDEO_FORMAT_YVYU));
	assert(run_format(VIDEO_FORMAT_UYVY));
	return 0;
}
