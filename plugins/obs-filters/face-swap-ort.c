/* Minimal ONNX Runtime loader that does not require the optional ORT SDK. */

#include "face-swap-ort.h"

#include <obs-module.h>
#include <util/platform.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(FACE_SWAP_HAS_ORT)
#include <onnxruntime_c_api.h>
#endif

/* OrtGetApiBase is ONNX Runtime's stable C ABI entry point.  Keep this small
 * prefix local so merely detecting the runtime never couples OBS to an ORT
 * import library.  Session access will use the official header once the SDK
 * and model contract are pinned. */
struct ort_api_base {
	const void *(*get_api)(uint32_t version);
	const char *(*get_version_string)(void);
};

typedef const struct ort_api_base *(*ort_get_api_base_t)(void);

bool face_swap_ort_load(struct face_swap_ort *ort)
{
	if (!ort)
		return false;

	ort->module = os_dlopen("onnxruntime.dll");
	if (!ort->module)
		return false;

	ort_get_api_base_t get_api_base = (ort_get_api_base_t)os_dlsym(ort->module, "OrtGetApiBase");
	if (!get_api_base)
		goto fail;

	const struct ort_api_base *base = get_api_base();
	if (!base || !base->get_api || !base->get_version_string)
		goto fail;

	ort->version = base->get_version_string();
	if (!ort->version || !*ort->version)
		goto fail;

#if defined(FACE_SWAP_HAS_ORT)
	ort->api = base->get_api(ORT_API_VERSION);
	if (!ort->api)
		goto fail;

	const OrtApi *api = ort->api;
	OrtStatus *status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "obs-face-swap", (OrtEnv **)&ort->env);
	if (status) {
		blog(LOG_WARNING, "[face swap] ONNX Runtime environment creation failed: %s",
		     api->GetErrorMessage(status));
		api->ReleaseStatus(status);
		goto fail;
	}
	ort->sdk_available = true;
#else
	ort->api = NULL;
	ort->sdk_available = false;
#endif
	return true;

fail:
	face_swap_ort_unload(ort);
	return false;
}

bool face_swap_ort_create_cpu_session(struct face_swap_ort *ort, const wchar_t *model_path,
				      const int64_t expected_shapes[][4], const size_t *expected_ranks, size_t expected_count,
				      struct face_swap_ort_session *out)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!ort || !ort->sdk_available || !ort->api || !ort->env || !model_path || !*model_path || !expected_shapes ||
	    !expected_ranks || !expected_count || expected_count > 2 || !out)
		return false;
	memset(out, 0, sizeof(*out));

	const OrtApi *api = ort->api;
	OrtSessionOptions *options = NULL;
	OrtSession *session = NULL;
	OrtStatus *status = api->CreateSessionOptions(&options);
	if (!status)
		status = api->SetIntraOpNumThreads(options, 1);
	if (!status)
		status = api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
	if (!status)
		status = api->CreateSession((const OrtEnv *)ort->env, model_path, options, &session);

	if (options)
		api->ReleaseSessionOptions(options);
	if (status) {
		blog(LOG_WARNING, "[face swap] ONNX Runtime CPU session creation failed: %s",
		     api->GetErrorMessage(status));
		api->ReleaseStatus(status);
		return false;
	}

	size_t input_count = 0;
	OrtAllocator *allocator = NULL;
	OrtTypeInfo *type_info = NULL;
	const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
	char *input_name = NULL;
	char **output_names = NULL;
	size_t output_count = 0;
	enum ONNXTensorElementDataType element_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
	size_t rank = 0;

	status = api->SessionGetInputCount(session, &input_count);
	if (!status && input_count != expected_count) {
		blog(LOG_WARNING, "[face swap] expected %zu model inputs, found %zu", expected_count, input_count);
		goto invalid_model;
	}
	if (!status)
		status = api->GetAllocatorWithDefaultOptions(&allocator);
	for (size_t input_index = 0; !status && input_index < input_count; input_index++) {
		status = api->SessionGetInputName(session, input_index, allocator, &input_name);
		if (!status)
			status = api->SessionGetInputTypeInfo(session, input_index, &type_info);
		if (!status)
			status = api->CastTypeInfoToTensorInfo(type_info, &tensor_info);
		if (!status && !tensor_info)
			goto invalid_model;
		if (!status)
			status = api->GetTensorElementType(tensor_info, &element_type);
		if (!status && element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			goto invalid_model;
		if (!status)
			status = api->GetDimensionsCount(tensor_info, &rank);
		if (!status && rank != expected_ranks[input_index])
			goto invalid_model;
		if (!status)
			status = api->GetDimensions(tensor_info, out->input_shapes[input_index], rank);
		if (!status) {
			for (size_t dim = 0; dim < rank; dim++) {
				if (out->input_shapes[input_index][dim] != -1 &&
				    out->input_shapes[input_index][dim] != expected_shapes[input_index][dim])
					goto invalid_model;
				out->input_shapes[input_index][dim] = expected_shapes[input_index][dim];
			}
		}
		out->input_names[input_index] = bstrdup(input_name);
		if (!out->input_names[input_index])
			goto invalid_model;
		out->input_ranks[input_index] = rank;
		api->AllocatorFree(allocator, input_name);
		input_name = NULL;
		api->ReleaseTypeInfo(type_info);
		type_info = NULL;
		tensor_info = NULL;
	}
	if (!status)
		status = api->SessionGetOutputCount(session, &output_count);
	if (!status && (!output_count || output_count > 64)) {
		blog(LOG_WARNING, "[face swap] invalid model output count: %zu", output_count);
		goto invalid_model;
	}
	if (!status)
		output_names = bzalloc(output_count * sizeof(*output_names));
	if (!status && !output_names)
		goto invalid_model;
	for (size_t i = 0; !status && i < output_count; i++) {
		char *name = NULL;
		status = api->SessionGetOutputName(session, i, allocator, &name);
		if (!status) {
			output_names[i] = bstrdup(name);
			api->AllocatorFree(allocator, name);
			if (!output_names[i])
				goto invalid_model;
		}
	}
	if (status) {
		blog(LOG_WARNING, "[face swap] model input inspection failed: %s", api->GetErrorMessage(status));
		api->ReleaseStatus(status);
		status = NULL;
		goto invalid_model;
	}

	out->session = session;
	out->input_count = input_count;
	out->output_names = output_names;
	out->output_count = output_count;
	return true;

invalid_model:
	if (status) {
		blog(LOG_WARNING, "[face swap] model input inspection failed: %s", api->GetErrorMessage(status));
		api->ReleaseStatus(status);
	}
	if (input_name && allocator)
		api->AllocatorFree(allocator, input_name);
	if (type_info)
		api->ReleaseTypeInfo(type_info);
	for (size_t i = 0; i < 2; i++)
		bfree(out->input_names[i]);
	if (output_names) {
		for (size_t i = 0; i < output_count; i++)
			bfree(output_names[i]);
		bfree(output_names);
	}
	api->ReleaseSession(session);
	memset(out, 0, sizeof(*out));
	return false;
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(model_path);
	UNUSED_PARAMETER(expected_shapes);
	UNUSED_PARAMETER(expected_ranks);
	UNUSED_PARAMETER(expected_count);
	UNUSED_PARAMETER(out);
	return false;
#endif
}

void face_swap_ort_release_session(struct face_swap_ort *ort, struct face_swap_ort_session *session)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!session)
		return;
	if (ort && ort->api && session->session)
		((const OrtApi *)ort->api)->ReleaseSession((OrtSession *)session->session);
	for (size_t i = 0; i < session->input_count; i++)
		bfree(session->input_names[i]);
	for (size_t i = 0; i < session->output_count; i++)
		bfree(session->output_names[i]);
	bfree(session->output_names);
	memset(session, 0, sizeof(*session));
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(session);
#endif
}

static bool face_swap_ort_run_inputs(struct face_swap_ort *ort, struct face_swap_ort_session *session,
				     float *const *input_data, const size_t *input_counts, size_t input_count,
				     face_swap_ort_output_callback callback, void *param)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!ort || !ort->api || !session || !session->session || session->input_count != input_count || !input_data ||
	    !input_counts || !session->output_count || !input_count || input_count > 2)
		return false;
	for (size_t input = 0; input < input_count; input++) {
		if (!session->input_names[input] || !input_data[input] || input_counts[input] > SIZE_MAX / sizeof(float))
			return false;
		size_t expected_count = 1;
		for (size_t dim = 0; dim < session->input_ranks[input]; dim++) {
			if (session->input_shapes[input][dim] <= 0 ||
			    (size_t)session->input_shapes[input][dim] > SIZE_MAX / expected_count)
				return false;
			expected_count *= (size_t)session->input_shapes[input][dim];
		}
		if (input_counts[input] != expected_count)
			return false;
	}

	const OrtApi *api = ort->api;
	OrtMemoryInfo *memory_info = NULL;
	OrtValue *input_values[2] = {0};
	OrtValue **outputs = bzalloc(session->output_count * sizeof(*outputs));
	struct face_swap_ort_tensor *views = bzalloc(session->output_count * sizeof(*views));
	if (!outputs || !views) {
		bfree(outputs);
		bfree(views);
		return false;
	}

	OrtStatus *status = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info);
	for (size_t input = 0; !status && input < input_count; input++)
		status = api->CreateTensorWithDataAsOrtValue(
			memory_info, input_data[input], input_counts[input] * sizeof(float), session->input_shapes[input],
			session->input_ranks[input], ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_values[input]);
	int is_tensor = 0;
	for (size_t input = 0; !status && input < input_count; input++) {
		status = api->IsTensor(input_values[input], &is_tensor);
		if (!status && !is_tensor)
			goto cleanup;
	}
	if (!status) {
		status = api->Run((OrtSession *)session->session, NULL, (const char *const *)session->input_names,
				  (const OrtValue *const *)input_values, input_count,
				  (const char *const *)session->output_names, session->output_count, outputs);
	}
	for (size_t i = 0; !status && i < session->output_count; i++) {
		int output_is_tensor = 0;
		status = api->IsTensor(outputs[i], &output_is_tensor);
		if (!status && !output_is_tensor) {
			is_tensor = 0;
			break;
		}

		OrtTensorTypeAndShapeInfo *shape_info = NULL;
		if (!status)
			status = api->GetTensorTypeAndShape(outputs[i], &shape_info);
		enum ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
		if (!status)
			status = api->GetTensorElementType(shape_info, &type);
		if (!status && type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
			is_tensor = 0;
			api->ReleaseTensorTypeAndShapeInfo(shape_info);
			break;
		}
		if (!status)
			status = api->GetDimensionsCount(shape_info, &views[i].rank);
		if (!status && views[i].rank > FACE_SWAP_ORT_MAX_RANK) {
			is_tensor = 0;
			api->ReleaseTensorTypeAndShapeInfo(shape_info);
			break;
		}
		if (!status)
			status = api->GetDimensions(shape_info, views[i].shape, views[i].rank);
		if (!status)
			status = api->GetTensorShapeElementCount(shape_info, &views[i].element_count);
		if (!status)
			status = api->GetTensorMutableData(outputs[i], (void **)&views[i].data);
		views[i].name = session->output_names[i];
		if (shape_info)
			api->ReleaseTensorTypeAndShapeInfo(shape_info);
	}
	if (!status && is_tensor && callback)
		is_tensor = callback(param, views, session->output_count);

cleanup:
	const bool success = status == NULL && is_tensor;
	if (status)
		api->ReleaseStatus(status);
	for (size_t i = 0; i < session->output_count; i++) {
		if (outputs[i])
			api->ReleaseValue(outputs[i]);
	}
	for (size_t input = 0; input < input_count; input++) {
		if (input_values[input])
			api->ReleaseValue(input_values[input]);
	}
	if (memory_info)
		api->ReleaseMemoryInfo(memory_info);
	bfree(outputs);
	bfree(views);
	return success;
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(input_data);
	UNUSED_PARAMETER(input_counts);
	UNUSED_PARAMETER(input_count);
	UNUSED_PARAMETER(callback);
	UNUSED_PARAMETER(param);
	return false;
#endif
}

bool face_swap_ort_run_float(struct face_swap_ort *ort, struct face_swap_ort_session *session, float *input,
			     size_t input_count, face_swap_ort_output_callback callback, void *param)
{
	float *inputs[] = {input};
	const size_t counts[] = {input_count};
	return face_swap_ort_run_inputs(ort, session, inputs, counts, 1, callback, param);
}

void face_swap_ort_unload(struct face_swap_ort *ort)
{
	if (!ort)
		return;

#if defined(FACE_SWAP_HAS_ORT)
	if (ort->api && ort->env)
		((const OrtApi *)ort->api)->ReleaseEnv((OrtEnv *)ort->env);
#endif

	if (ort->module)
		os_dlclose(ort->module);

	ort->module = NULL;
	ort->api = NULL;
	ort->env = NULL;
	ort->version = NULL;
	ort->sdk_available = false;
}
