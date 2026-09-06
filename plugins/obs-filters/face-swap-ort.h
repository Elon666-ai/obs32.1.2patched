#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

struct face_swap_ort {
	void *module;
	const void *api;
	void *env;
	const char *version;
	bool sdk_available;
};

struct face_swap_ort_session {
	void *session;
	size_t input_count;
	char *input_names[2];
	int64_t input_shapes[2][4];
	size_t input_ranks[2];
	char **output_names;
	size_t output_count;
};

#define FACE_SWAP_ORT_MAX_RANK 4

struct face_swap_ort_tensor {
	const char *name;
	const float *data;
	int64_t shape[FACE_SWAP_ORT_MAX_RANK];
	size_t rank;
	size_t element_count;
};

typedef bool (*face_swap_ort_output_callback)(void *param, const struct face_swap_ort_tensor *outputs,
					     size_t output_count);

bool face_swap_ort_load(struct face_swap_ort *ort);
bool face_swap_ort_create_cpu_session(struct face_swap_ort *ort, const wchar_t *model_path,
				      const int64_t expected_shapes[][4], const size_t *expected_ranks, size_t expected_count,
				      struct face_swap_ort_session *out);
void face_swap_ort_release_session(struct face_swap_ort *ort, struct face_swap_ort_session *session);
bool face_swap_ort_run_float(struct face_swap_ort *ort, struct face_swap_ort_session *session, float *input,
			     size_t input_count, face_swap_ort_output_callback callback, void *param);
void face_swap_ort_unload(struct face_swap_ort *ort);
