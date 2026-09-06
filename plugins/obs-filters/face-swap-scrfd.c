#include "face-swap-scrfd.h"

#include <math.h>
#include <string.h>

#define SCRFD_INPUT_SIZE 640
#define SCRFD_THRESHOLD 0.5f

struct scrfd_group {
	const struct face_swap_ort_tensor *score;
	const struct face_swap_ort_tensor *bbox;
	const struct face_swap_ort_tensor *landmark;
	size_t anchors;
	uint32_t stride;
};

static size_t tensor_rows(const struct face_swap_ort_tensor *tensor, size_t columns)
{
	return tensor && columns && tensor->element_count % columns == 0 ? tensor->element_count / columns : 0;
}

static uint32_t infer_stride(size_t anchors)
{
	const uint32_t strides[] = {8, 16, 32};
	for (size_t i = 0; i < 3; i++) {
		const size_t side = SCRFD_INPUT_SIZE / strides[i];
		if (anchors == side * side * 2)
			return strides[i];
	}
	return 0;
}

static bool build_groups(struct scrfd_group groups[3], const struct face_swap_ort_tensor *outputs,
			 size_t output_count)
{
	if (!outputs || output_count != 9)
		return false;

	for (size_t i = 0; i < output_count; i++) {
		const struct face_swap_ort_tensor *tensor = &outputs[i];
		if (!tensor->data || !tensor->rank || tensor->rank > FACE_SWAP_ORT_MAX_RANK)
			return false;
		size_t columns = 0;
		if (tensor->element_count && tensor->shape[tensor->rank - 1] == 1)
			columns = 1;
		else if (tensor->rank && tensor->shape[tensor->rank - 1] == 4)
			columns = 4;
		else if (tensor->rank && tensor->shape[tensor->rank - 1] == 10)
			columns = 10;
		if (!columns)
			return false;

		const size_t anchors = tensor_rows(tensor, columns);
		const uint32_t stride = infer_stride(anchors);
		if (!stride)
			return false;
		const size_t group_index = stride == 8 ? 0 : stride == 16 ? 1 : 2;
		groups[group_index].anchors = anchors;
		groups[group_index].stride = stride;
		if (columns == 1)
			groups[group_index].score = tensor;
		else if (columns == 4)
			groups[group_index].bbox = tensor;
		else
			groups[group_index].landmark = tensor;
	}

	for (size_t i = 0; i < 3; i++) {
		if (!groups[i].score || !groups[i].bbox || !groups[i].landmark)
			return false;
	}
	return true;
}

bool face_swap_scrfd_decode(void *param, const struct face_swap_ort_tensor *outputs, size_t output_count)
{
	struct face_swap_detection *best = param;
	struct scrfd_group groups[3] = {0};
	if (!best || !build_groups(groups, outputs, output_count))
		return false;

	memset(best, 0, sizeof(*best));
	for (size_t g = 0; g < 3; g++) {
		const struct scrfd_group *group = &groups[g];
		const uint32_t side = SCRFD_INPUT_SIZE / group->stride;
		for (size_t i = 0; i < group->anchors; i++) {
			const float score = group->score->data[i];
			if (!isfinite(score) || score < SCRFD_THRESHOLD || score <= best->score)
				continue;

			const size_t cell = i / 2;
			const float cx = (float)(cell % side) * group->stride;
			const float cy = (float)(cell / side) * group->stride;
			const float *bbox = group->bbox->data + i * 4;
			const float *landmark = group->landmark->data + i * 10;
			bool valid = true;
			for (size_t n = 0; n < 4; n++)
				valid = valid && isfinite(bbox[n]) && bbox[n] >= 0.0f;
			for (size_t n = 0; n < 10; n++)
				valid = valid && isfinite(landmark[n]);
			if (!valid)
				continue;

			best->x1 = fmaxf(0.0f, cx - bbox[0] * group->stride);
			best->y1 = fmaxf(0.0f, cy - bbox[1] * group->stride);
			best->x2 = fminf(SCRFD_INPUT_SIZE - 1.0f, cx + bbox[2] * group->stride);
			best->y2 = fminf(SCRFD_INPUT_SIZE - 1.0f, cy + bbox[3] * group->stride);
			if (best->x2 <= best->x1 || best->y2 <= best->y1)
				continue;
			for (size_t n = 0; n < 5; n++) {
				best->landmarks[n * 2] = cx + landmark[n * 2] * group->stride;
				best->landmarks[n * 2 + 1] = cy + landmark[n * 2 + 1] * group->stride;
			}
			best->score = score;
		}
	}

	/* A valid layout with no face is still a successful decode. */
	return true;
}

static float clamp_coord(float value, float maximum)
{
	return fminf(maximum, fmaxf(0.0f, value));
}

bool face_swap_detection_unletterbox(struct face_swap_detection *detection,
				     const struct face_swap_letterbox *letterbox)
{
	if (!detection || !letterbox || detection->score <= 0.0f || letterbox->scale <= 0.0f ||
	    !letterbox->source_width || !letterbox->source_height)
		return false;

	const float max_x = letterbox->source_width - 1.0f;
	const float max_y = letterbox->source_height - 1.0f;
	detection->x1 = clamp_coord((detection->x1 - letterbox->pad_x) / letterbox->scale, max_x);
	detection->y1 = clamp_coord((detection->y1 - letterbox->pad_y) / letterbox->scale, max_y);
	detection->x2 = clamp_coord((detection->x2 - letterbox->pad_x) / letterbox->scale, max_x);
	detection->y2 = clamp_coord((detection->y2 - letterbox->pad_y) / letterbox->scale, max_y);
	if (detection->x2 <= detection->x1 || detection->y2 <= detection->y1) {
		detection->score = 0.0f;
		return false;
	}

	for (size_t i = 0; i < 5; i++) {
		detection->landmarks[i * 2] =
			clamp_coord((detection->landmarks[i * 2] - letterbox->pad_x) / letterbox->scale, max_x);
		detection->landmarks[i * 2 + 1] =
			clamp_coord((detection->landmarks[i * 2 + 1] - letterbox->pad_y) / letterbox->scale, max_y);
	}
	return true;
}
