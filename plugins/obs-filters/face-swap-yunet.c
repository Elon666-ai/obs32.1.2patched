#include "face-swap-yunet.h"

#include <math.h>
#include <string.h>

#define YUNET_INPUT_SIZE 640
#define YUNET_THRESHOLD 0.6f
#define YUNET_GROUPS 3

struct yunet_group {
	const struct face_swap_ort_tensor *cls;
	const struct face_swap_ort_tensor *obj;
	const struct face_swap_ort_tensor *bbox;
	const struct face_swap_ort_tensor *kps;
	uint32_t stride;
	size_t anchors;
};

static uint32_t stride_for(size_t anchors)
{
	static const uint32_t strides[YUNET_GROUPS] = {8, 16, 32};
	for (size_t i = 0; i < YUNET_GROUPS; i++) {
		const size_t side = YUNET_INPUT_SIZE / strides[i];
		if (anchors == side * side)
			return strides[i];
	}
	return 0;
}

/* Sorts the 12 tensors into per-stride groups using shape alone, so the model's
 * output names are never relied on. */
static bool build_groups(struct yunet_group groups[YUNET_GROUPS], const struct face_swap_ort_tensor *outputs,
			 size_t output_count)
{
	if (!outputs || output_count != 12)
		return false;

	memset(groups, 0, sizeof(struct yunet_group) * YUNET_GROUPS);

	for (size_t i = 0; i < output_count; i++) {
		const struct face_swap_ort_tensor *tensor = &outputs[i];
		if (!tensor->data || !tensor->rank || tensor->rank > FACE_SWAP_ORT_MAX_RANK)
			return false;

		const int64_t columns = tensor->shape[tensor->rank - 1];
		if (columns != 1 && columns != 4 && columns != 10)
			return false;
		if (tensor->element_count % (size_t)columns != 0)
			return false;

		const size_t anchors = tensor->element_count / (size_t)columns;
		const uint32_t stride = stride_for(anchors);
		if (!stride)
			return false;

		struct yunet_group *group = &groups[stride == 8 ? 0 : stride == 16 ? 1 : 2];
		group->stride = stride;
		group->anchors = anchors;

		if (columns == 4) {
			group->bbox = tensor;
		} else if (columns == 10) {
			group->kps = tensor;
		} else {
			/* cls and obj share the [N, 1] shape; the first one seen
			 * is cls, matching the model's output order. */
			if (!group->cls)
				group->cls = tensor;
			else if (!group->obj)
				group->obj = tensor;
			else
				return false;
		}
	}

	for (size_t i = 0; i < YUNET_GROUPS; i++) {
		if (!groups[i].cls || !groups[i].obj || !groups[i].bbox || !groups[i].kps)
			return false;
	}
	return true;
}

bool face_swap_yunet_decode(void *param, const struct face_swap_ort_tensor *outputs, size_t output_count)
{
	struct face_swap_detection *best = param;
	struct yunet_group groups[YUNET_GROUPS];
	if (!best || !build_groups(groups, outputs, output_count))
		return false;

	memset(best, 0, sizeof(*best));

	for (size_t g = 0; g < YUNET_GROUPS; g++) {
		const struct yunet_group *group = &groups[g];
		const uint32_t side = YUNET_INPUT_SIZE / group->stride;

		for (size_t i = 0; i < group->anchors; i++) {
			const float cls = group->cls->data[i];
			const float obj = group->obj->data[i];
			if (!isfinite(cls) || !isfinite(obj))
				continue;

			/* Both tensors are already sigmoid-activated. */
			const float score = sqrtf(fmaxf(0.0f, cls) * fmaxf(0.0f, obj));
			if (score < YUNET_THRESHOLD || score <= best->score)
				continue;

			const float cx = (float)(i % side);
			const float cy = (float)(i / side);
			const float *bbox = group->bbox->data + i * 4;
			const float *kps = group->kps->data + i * 10;

			bool valid = true;
			for (size_t n = 0; n < 4; n++)
				valid = valid && isfinite(bbox[n]);
			for (size_t n = 0; n < 10; n++)
				valid = valid && isfinite(kps[n]);
			if (!valid)
				continue;

			/* Offsets are in stride units relative to the cell origin;
			 * width and height are stored as natural logarithms. */
			const float x = (cx + bbox[0]) * group->stride;
			const float y = (cy + bbox[1]) * group->stride;
			const float w = expf(bbox[2]) * group->stride;
			const float h = expf(bbox[3]) * group->stride;
			if (!(w > 1.0f) || !(h > 1.0f) || w > YUNET_INPUT_SIZE * 2.0f || h > YUNET_INPUT_SIZE * 2.0f)
				continue;

			best->x1 = fmaxf(0.0f, x);
			best->y1 = fmaxf(0.0f, y);
			best->x2 = fminf(YUNET_INPUT_SIZE - 1.0f, x + w);
			best->y2 = fminf(YUNET_INPUT_SIZE - 1.0f, y + h);
			if (best->x2 <= best->x1 || best->y2 <= best->y1)
				continue;

			for (size_t n = 0; n < 5; n++) {
				best->landmarks[n * 2] = (cx + kps[n * 2]) * group->stride;
				best->landmarks[n * 2 + 1] = (cy + kps[n * 2 + 1]) * group->stride;
			}
			best->score = score;
		}
	}

	/* A valid layout with no face above threshold is still a successful decode. */
	return true;
}
