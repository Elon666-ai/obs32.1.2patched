#include "face-swap-stabilize.h"

#include <math.h>
#include <string.h>

#define STABILIZER_ALPHA 0.35f
#define STABILIZER_MAX_JUMP 0.45f
#define STABILIZER_HOLD_FRAMES 6
#define STABILIZER_FADE_IN 0.2f
#define STABILIZER_FADE_OUT 0.16f

static float smooth_value(float previous, float current, float alpha)
{
	return previous + (current - previous) * alpha;
}

static float box_width(const struct face_swap_detection *detection)
{
	return fmaxf(1.0f, detection->x2 - detection->x1);
}

static float box_height(const struct face_swap_detection *detection)
{
	return fmaxf(1.0f, detection->y2 - detection->y1);
}

static bool jump_is_reasonable(const struct face_swap_detection *previous,
				       const struct face_swap_detection *current)
{
	const float width = box_width(previous);
	const float height = box_height(previous);
	const float center_x = (previous->x1 + previous->x2) * 0.5f;
	const float center_y = (previous->y1 + previous->y2) * 0.5f;
	const float current_center_x = (current->x1 + current->x2) * 0.5f;
	const float current_center_y = (current->y1 + current->y2) * 0.5f;

	return fabsf(current_center_x - center_x) <= width * STABILIZER_MAX_JUMP &&
	       fabsf(current_center_y - center_y) <= height * STABILIZER_MAX_JUMP &&
	       current->x2 - current->x1 <= width * 2.0f && current->y2 - current->y1 <= height * 2.0f;
}

void face_swap_stabilizer_reset(struct face_swap_stabilizer *stabilizer)
{
	if (stabilizer)
		memset(stabilizer, 0, sizeof(*stabilizer));
}

void face_swap_stabilizer_update(struct face_swap_stabilizer *stabilizer,
				 const struct face_swap_detection *detection)
{
	if (!stabilizer || !detection)
		return;

	const bool detected = detection->score > 0.0f && isfinite(detection->score);
	if (detected && (!stabilizer->valid || jump_is_reasonable(&stabilizer->value, detection))) {
		if (!stabilizer->valid)
			stabilizer->value = *detection;
		else {
			stabilizer->value.x1 = smooth_value(stabilizer->value.x1, detection->x1, STABILIZER_ALPHA);
			stabilizer->value.y1 = smooth_value(stabilizer->value.y1, detection->y1, STABILIZER_ALPHA);
			stabilizer->value.x2 = smooth_value(stabilizer->value.x2, detection->x2, STABILIZER_ALPHA);
			stabilizer->value.y2 = smooth_value(stabilizer->value.y2, detection->y2, STABILIZER_ALPHA);
			for (size_t i = 0; i < 10; i++)
				stabilizer->value.landmarks[i] =
					smooth_value(stabilizer->value.landmarks[i], detection->landmarks[i], STABILIZER_ALPHA);
			stabilizer->value.score = detection->score;
		}
		stabilizer->valid = true;
		stabilizer->stable_frames++;
		stabilizer->lost_frames = 0;
		stabilizer->opacity = fminf(1.0f, stabilizer->opacity + STABILIZER_FADE_IN);
	} else if (stabilizer->valid) {
		stabilizer->lost_frames++;
		if (stabilizer->lost_frames > STABILIZER_HOLD_FRAMES)
			stabilizer->opacity = fmaxf(0.0f, stabilizer->opacity - STABILIZER_FADE_OUT);
		if (stabilizer->opacity <= 0.0f) {
			stabilizer->valid = false;
			stabilizer->stable_frames = 0;
		}
	}
}
