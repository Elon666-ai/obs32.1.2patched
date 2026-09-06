#include "face-swap-debug.h"

#include <math.h>

static uint8_t *luma_pixel(struct obs_source_frame *frame, uint32_t x, uint32_t y)
{
	uint8_t *row = frame->data[0] + (size_t)y * frame->linesize[0];
	switch (frame->format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
		return row + x;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
		return row + x * 2;
	case VIDEO_FORMAT_UYVY:
		return row + x * 2 + 1;
	default:
		return NULL;
	}
}

static void set_luma(struct obs_source_frame *frame, uint32_t x, uint32_t y, uint8_t value)
{
	uint8_t *pixel = luma_pixel(frame, x, y);
	if (pixel)
		*pixel = value;
}

bool face_swap_draw_detection(struct obs_source_frame *frame, const struct face_swap_detection *detection,
			      float opacity)
{
	if (!frame || !detection || !frame->data[0] || detection->score <= 0.0f || opacity <= 0.0f ||
	    !frame->width || !frame->height)
		return false;

	if (frame->format != VIDEO_FORMAT_I420 && frame->format != VIDEO_FORMAT_NV12 &&
	    frame->format != VIDEO_FORMAT_YUY2 && frame->format != VIDEO_FORMAT_YVYU &&
	    frame->format != VIDEO_FORMAT_UYVY)
		return false;

	const uint32_t x1 = (uint32_t)fminf(frame->width - 1.0f, fmaxf(0.0f, detection->x1));
	const uint32_t y1 = (uint32_t)fminf(frame->height - 1.0f, fmaxf(0.0f, detection->y1));
	const uint32_t x2 = (uint32_t)fminf(frame->width - 1.0f, fmaxf(0.0f, detection->x2));
	const uint32_t y2 = (uint32_t)fminf(frame->height - 1.0f, fmaxf(0.0f, detection->y2));
	if (x2 <= x1 || y2 <= y1)
		return false;

	const uint8_t value = (uint8_t)(160.0f + fminf(1.0f, opacity) * 95.0f);
	for (uint32_t x = x1; x <= x2; x++) {
		set_luma(frame, x, y1, value);
		set_luma(frame, x, y2, value);
	}
	for (uint32_t y = y1; y <= y2; y++) {
		set_luma(frame, x1, y, value);
		set_luma(frame, x2, y, value);
	}

	for (size_t i = 0; i < 5; i++) {
		const uint32_t x = (uint32_t)fminf(frame->width - 1.0f, fmaxf(0.0f, detection->landmarks[i * 2]));
		const uint32_t y = (uint32_t)fminf(frame->height - 1.0f, fmaxf(0.0f, detection->landmarks[i * 2 + 1]));
		set_luma(frame, x, y, value);
	}
	return true;
}
