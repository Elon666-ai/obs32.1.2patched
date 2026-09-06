#include "face-swap-preprocess.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void identity_matrix(float matrix[16])
{
	memset(matrix, 0, sizeof(float) * 16);
	matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static void yuv_channel_matrix(float matrix[16])
{
	memset(matrix, 0, sizeof(float) * 16);
	matrix[2] = 1.0f;  /* R = V */
	matrix[4] = 1.0f;  /* G = Y */
	matrix[9] = 1.0f;  /* B = U */
	matrix[15] = 1.0f;
}

static void test_i420_flip(void)
{
	uint8_t y[] = {10, 20, 30, 40};
	uint8_t u[] = {128};
	uint8_t v[] = {128};
	uint8_t rgb[12];
	struct obs_source_frame frame = {0};
	frame.data[0] = y;
	frame.data[1] = u;
	frame.data[2] = v;
	frame.linesize[0] = 2;
	frame.linesize[1] = 1;
	frame.linesize[2] = 1;
	frame.width = frame.height = 2;
	frame.format = VIDEO_FORMAT_I420;
	frame.flip = true;
	identity_matrix(frame.color_matrix);

	assert(face_swap_frame_to_rgb(rgb, sizeof(rgb), &frame));
	assert(rgb[0] == 30 && rgb[3] == 40);
	assert(rgb[6] == 10 && rgb[9] == 20);
}

static void test_packed422(void)
{
	uint8_t packed[] = {10, 30, 20, 40};
	uint8_t rgb[6];
	struct obs_source_frame frame = {0};
	frame.data[0] = packed;
	frame.linesize[0] = sizeof(packed);
	frame.width = 2;
	frame.height = 1;
	frame.format = VIDEO_FORMAT_YUY2;
	yuv_channel_matrix(frame.color_matrix);
	assert(face_swap_frame_to_rgb(rgb, sizeof(rgb), &frame));
	assert(rgb[0] == 40 && rgb[1] == 10 && rgb[2] == 30);
	assert(rgb[3] == 40 && rgb[4] == 20 && rgb[5] == 30);

	frame.format = VIDEO_FORMAT_YVYU;
	assert(face_swap_frame_to_rgb(rgb, sizeof(rgb), &frame));
	assert(rgb[0] == 30 && rgb[1] == 10 && rgb[2] == 40);

	packed[0] = 30;
	packed[1] = 10;
	packed[2] = 40;
	packed[3] = 20;
	frame.format = VIDEO_FORMAT_UYVY;
	assert(face_swap_frame_to_rgb(rgb, sizeof(rgb), &frame));
	assert(rgb[0] == 40 && rgb[1] == 10 && rgb[2] == 30);

	frame.linesize[0] = 3;
	assert(!face_swap_frame_to_rgb(rgb, sizeof(rgb), &frame));
}

static void test_tensor_pack(void)
{
	const uint8_t rgb[] = {
		0, 10, 20, 100, 110, 120,
		200, 210, 220, 240, 250, 255,
	};
	float chw[3];
	const struct face_swap_tensor_params params = {
		.mean = {10.0f, 10.0f, 10.0f},
		.scale = {0.5f, 1.0f, 2.0f},
		.bgr = true,
	};

	assert(face_swap_rgb_to_chw(chw, 3, rgb, 2, 2, 1, 1, &params));
	/* Center sample averages the four source pixels: RGB = 135,145,153.75;
	 * BGR output then applies per-output-channel normalization. */
	assert(chw[0] > 71.8f && chw[0] < 71.9f);
	assert(chw[1] == 135.0f);
	assert(chw[2] == 250.0f);
	assert(!face_swap_rgb_to_chw(chw, 2, rgb, 2, 2, 1, 1, &params));
}

static void test_letterbox(void)
{
	const uint8_t rgb[] = {100, 110, 120, 100, 110, 120, 100, 110, 120, 100, 110, 120};
	float chw[3 * 4];
	struct face_swap_letterbox letterbox;
	const struct face_swap_tensor_params params = {
		.mean = {0.0f, 0.0f, 0.0f},
		.scale = {1.0f, 1.0f, 1.0f},
		.bgr = false,
	};
	assert(face_swap_rgb_to_letterbox_chw(chw, 12, rgb, 2, 2, 2, 2, &params, &letterbox));
	assert(letterbox.scale == 1.0f && letterbox.pad_x == 0.0f && letterbox.pad_y == 0.0f);
	assert(chw[0] == 100.0f && chw[4] == 110.0f && chw[8] == 120.0f);

	const uint8_t landscape[] = {
		10, 20, 30, 10, 20, 30, 10, 20, 30, 10, 20, 30,
		10, 20, 30, 10, 20, 30, 10, 20, 30, 10, 20, 30,
	};
	float landscape_chw[3 * 16];
	assert(face_swap_rgb_to_letterbox_chw(landscape_chw, 48, landscape, 4, 2, 4, 4, &params, &letterbox));
	assert(letterbox.scale == 1.0f && letterbox.pad_x == 0.0f && letterbox.pad_y == 1.0f);
	assert(landscape_chw[0] == 114.0f);
	assert(landscape_chw[4] == 10.0f);

	assert(face_swap_rgb_to_letterbox_chw(landscape_chw, 48, landscape, 2, 4, 4, 4, &params, &letterbox));
	assert(letterbox.scale == 1.0f && letterbox.pad_x == 1.0f && letterbox.pad_y == 0.0f);
	assert(landscape_chw[0] == 114.0f);
	assert(landscape_chw[1] == 10.0f);
}

static void test_nv12(void)
{
	uint8_t y[] = {10, 20, 30, 40};
	uint8_t uv[] = {128, 128};
	uint8_t rgb[12];
	struct obs_source_frame frame = {0};
	frame.data[0] = y;
	frame.data[1] = uv;
	frame.linesize[0] = 2;
	frame.linesize[1] = 2;
	frame.width = frame.height = 2;
	frame.format = VIDEO_FORMAT_NV12;
	identity_matrix(frame.color_matrix);

	assert(face_swap_frame_to_rgb(rgb, sizeof(rgb), &frame));
	assert(rgb[0] == 10 && rgb[3] == 20 && rgb[6] == 30 && rgb[9] == 40);
}

int main(void)
{
	test_i420_flip();
	test_packed422();
	test_nv12();
	test_tensor_pack();
	test_letterbox();
	return 0;
}
