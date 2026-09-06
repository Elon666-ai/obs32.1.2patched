#include "face-swap-stabilize.h"

#include <assert.h>
#include <string.h>

static struct face_swap_detection detection(float x, float y)
{
	struct face_swap_detection value = {
		.x1 = x,
		.y1 = y,
		.x2 = x + 100.0f,
		.y2 = y + 100.0f,
		.score = 0.9f,
	};
	for (size_t i = 0; i < 5; i++) {
		value.landmarks[i * 2] = x + 20.0f + (float)i;
		value.landmarks[i * 2 + 1] = y + 30.0f + (float)i;
	}
	return value;
}

int main(void)
{
	struct face_swap_stabilizer stabilizer;
	face_swap_stabilizer_reset(&stabilizer);
	struct face_swap_detection first = detection(100.0f, 100.0f);
	face_swap_stabilizer_update(&stabilizer, &first);
	assert(stabilizer.valid && stabilizer.stable_frames == 1 && stabilizer.opacity == 0.2f);

	struct face_swap_detection nearby = detection(110.0f, 100.0f);
	face_swap_stabilizer_update(&stabilizer, &nearby);
	assert(stabilizer.value.x1 == 103.5f && stabilizer.stable_frames == 2);

	struct face_swap_detection jump = detection(300.0f, 300.0f);
	face_swap_stabilizer_update(&stabilizer, &jump);
	assert(stabilizer.value.x1 == 103.5f && stabilizer.lost_frames == 1);

	struct face_swap_detection missing;
	memset(&missing, 0, sizeof(missing));
	for (size_t i = 0; i < 6; i++)
		face_swap_stabilizer_update(&stabilizer, &missing);
	assert(stabilizer.valid && stabilizer.opacity > 0.23f && stabilizer.opacity < 0.25f);
	for (size_t i = 0; i < 3; i++)
		face_swap_stabilizer_update(&stabilizer, &missing);
	assert(!stabilizer.valid && stabilizer.opacity == 0.0f);

	face_swap_stabilizer_update(&stabilizer, &first);
	assert(stabilizer.valid && stabilizer.stable_frames == 1);
	return 0;
}
