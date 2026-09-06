/*
 * Face stylization filter (CPU, async frame path).
 *
 * Detects a face with SCRFD, aligns it to a 128x128 crop, runs a single-input
 * image-to-image generator over that crop, and blends the result back into the
 * live frame through a feathered elliptical mask.
 *
 * The generator carries the target appearance in its own weights, so no source
 * portrait and no identity embedding are involved.  That keeps the filter to
 * models the operator trained themselves or obtained under a permissive
 * licence (stylized/animal faces and similar).
 *
 * Threading contract: filter_video never blocks and never allocates.  It
 * publishes a frame copy for the worker, takes a snapshot of the latest result
 * under try-lock, and rewrites only the pixels inside the face mask, so the
 * background always comes from the current frame even though inference ran on
 * an older one.  With no model, no result, or zero strength the filter is an
 * exact passthrough.
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <pthread.h>
#include <string.h>
#include <wchar.h>

#include "face-swap-ort.h"
#include "face-swap-preprocess.h"
#include "face-swap-scrfd.h"
#include "face-swap-yunet.h"
#include "face-swap-stabilize.h"
#include "face-swap-debug.h"
#include "face-swap-align.h"
#include "face-swap-output.h"
#include "face-swap-blend.h"

#define S_STYLE_MODEL "style_model"
#define S_STRENGTH "strength"
#define S_DETECTION_FPS "detection_fps"
#define S_EXECUTION_PROVIDER "execution_provider"
#define S_DEBUG_OVERLAY "debug_overlay"

#define FACE_SWAP_DETECT_SIZE 640
#define FACE_SWAP_MODEL_DIR "obs-studio/face-swap-models"
#define FACE_SWAP_STYLE_TENSOR (3 * FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE)

/* Supported detectors, tried in this order.  YuNet is preferred: it is MIT
 * licensed and therefore safe for commercial use, whereas the InsightFace
 * SCRFD weights are restricted to non-commercial research. */
struct face_swap_detector_spec {
	const char *file;
	const char *label;
	face_swap_ort_output_callback decode;
	struct face_swap_tensor_params params;
};

static const struct face_swap_detector_spec FACE_SWAP_DETECTORS[] = {
	{
		.file = "yunet.onnx",
		.label = "YuNet",
		.decode = face_swap_yunet_decode,
		/* YuNet consumes raw 0-255 RGB. */
		.params = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false},
	},
	{
		.file = "scrfd-500m.onnx",
		.label = "SCRFD",
		.decode = face_swap_scrfd_decode,
		.params = {{127.5f, 127.5f, 127.5f}, {1.0f / 128.0f, 1.0f / 128.0f, 1.0f / 128.0f}, false},
	},
};

#define FACE_SWAP_DETECTOR_COUNT (sizeof(FACE_SWAP_DETECTORS) / sizeof(FACE_SWAP_DETECTORS[0]))

enum face_swap_provider {
	FACE_SWAP_PROVIDER_AUTO,
	FACE_SWAP_PROVIDER_CUDA,
	FACE_SWAP_PROVIDER_DIRECTML,
	FACE_SWAP_PROVIDER_CPU,
};

struct face_swap_slot {
	struct obs_source_frame *frame;
	uint32_t width;
	uint32_t height;
	enum video_format format;
	bool published;
};

struct face_swap_data {
	obs_source_t *context;

	pthread_mutex_t mutex;
	os_event_t *wake_event;
	pthread_t worker;
	bool worker_started;
	bool stop;

	/* worker-owned inference state */
	struct face_swap_ort ort;
	struct face_swap_ort_session detector;
	struct face_swap_ort_session style;
	const struct face_swap_detector_spec *detector_spec;
	bool detector_ready;
	bool style_ready;
	char *loaded_style;
	bool warned_inference;
	struct face_swap_detection detection;
	struct face_swap_letterbox letterbox;
	struct face_swap_stabilizer stabilizer;
	struct face_swap_affine affine;
	uint8_t face_crop[FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE * 3];
	uint8_t styled_crop[FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE * 3];
	float style_tensor[FACE_SWAP_STYLE_TENSOR];
	bool styled_ready;
	uint64_t last_inference_timestamp;
	uint8_t *rgb;
	size_t rgb_size;
	float *detect_tensor;
	size_t detect_tensor_count;

	/* published result, guarded by mutex */
	uint8_t published_crop[FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE * 3];
	struct face_swap_affine published_affine;
	bool published_crop_ready;
	struct face_swap_detection published_detection;
	float published_opacity;
	uint64_t published_timestamp;
	uint32_t published_width;
	uint32_t published_height;

	/* video-thread scratch, only touched under the try-lock */
	uint8_t video_crop[FACE_SWAP_CROP_SIZE * FACE_SWAP_CROP_SIZE * 3];

	struct face_swap_slot input[2];
	int write_slot;
	int published_slot;
	uint32_t requested_width;
	uint32_t requested_height;
	enum video_format requested_format;
	uint64_t input_sequence;
	uint64_t consumed_sequence;

	char *style_model;
	double strength;
	int detection_fps;
	enum face_swap_provider provider;
	bool warned_unsupported_format;
	bool debug_overlay;
};

static const char *face_swap_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FaceSwapFilter");
}

static void face_swap_slot_reset(struct face_swap_slot *slot)
{
	if (slot->frame)
		obs_source_frame_destroy(slot->frame);

	memset(slot, 0, sizeof(*slot));
}

static bool face_swap_slot_matches(const struct face_swap_slot *slot, const struct obs_source_frame *frame)
{
	return slot->frame && slot->width == frame->width && slot->height == frame->height &&
	       slot->format == frame->format;
}

static bool face_swap_slot_prepare(struct face_swap_slot *slot, enum video_format format, uint32_t width,
				   uint32_t height)
{
	if (slot->frame && slot->width == width && slot->height == height && slot->format == format)
		return true;

	face_swap_slot_reset(slot);
	slot->frame = obs_source_frame_create(format, width, height);
	if (!slot->frame)
		return false;

	slot->width = width;
	slot->height = height;
	slot->format = format;
	return true;
}

static bool face_swap_supported_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY:
		return true;
	default:
		return false;
	}
}

/* Returns the absolute path of a file inside the model directory. */
static char *face_swap_model_path(const char *name)
{
	struct dstr relative = {0};
	dstr_printf(&relative, "%s/%s", FACE_SWAP_MODEL_DIR, name);
	char *path = os_get_config_path_ptr(relative.array);
	dstr_free(&relative);
	return path;
}

static bool face_swap_open_session(struct face_swap_ort *ort, const char *name, const int64_t shape[4],
				   struct face_swap_ort_session *out)
{
	char *path = face_swap_model_path(name);
	if (!path)
		return false;

	wchar_t *wide = NULL;
	bool opened = false;
	if (os_file_exists(path) && os_utf8_to_wcs_ptr(path, 0, &wide) && wide) {
		const int64_t shapes[1][4] = {{shape[0], shape[1], shape[2], shape[3]}};
		const size_t ranks[] = {4};
		opened = face_swap_ort_create_cpu_session(ort, wide, shapes, ranks, 1, out);
	}

	bfree(wide);
	bfree(path);
	return opened;
}

/* Loads the style generator named by the settings, releasing any previous one.
 * Called on the worker thread only. */
static void face_swap_reload_style(struct face_swap_data *filter, char *requested)
{
	face_swap_ort_release_session(&filter->ort, &filter->style);
	filter->style_ready = false;

	if (*requested) {
		const int64_t shape[4] = {1, 3, FACE_SWAP_CROP_SIZE, FACE_SWAP_CROP_SIZE};
		filter->style_ready = face_swap_open_session(&filter->ort, requested, shape, &filter->style);
		if (filter->style_ready)
			blog(LOG_INFO, "[face swap: '%s'] style model '%s' ready",
			     obs_source_get_name(filter->context), requested);
		else
			blog(LOG_WARNING, "[face swap: '%s'] style model '%s' unavailable or incompatible",
			     obs_source_get_name(filter->context), requested);
	}

	bfree(filter->loaded_style);
	filter->loaded_style = requested;
}

static void face_swap_run_inference(struct face_swap_data *filter, int work_slot)
{
	const uint32_t width = filter->input[work_slot].width;
	const uint32_t height = filter->input[work_slot].height;
	const size_t required_rgb = width <= SIZE_MAX / 3 / height ? (size_t)width * height * 3 : 0;

	memset(&filter->detection, 0, sizeof(filter->detection));
	filter->styled_ready = false;

	if (required_rgb && required_rgb > filter->rgb_size) {
		uint8_t *rgb = bmalloc(required_rgb);
		if (rgb) {
			bfree(filter->rgb);
			filter->rgb = rgb;
			filter->rgb_size = required_rgb;
		}
	}

	const size_t tensor_count = (size_t)FACE_SWAP_DETECT_SIZE * FACE_SWAP_DETECT_SIZE * 3;
	if (tensor_count > filter->detect_tensor_count) {
		float *tensor = bmalloc(tensor_count * sizeof(*tensor));
		if (tensor) {
			bfree(filter->detect_tensor);
			filter->detect_tensor = tensor;
			filter->detect_tensor_count = tensor_count;
		}
	}

	if (!required_rgb || !filter->rgb || !filter->detect_tensor)
		return;

	/* The producer owns the other slot, so this frame stays stable. */
	if (!face_swap_frame_to_rgb(filter->rgb, filter->rgb_size, filter->input[work_slot].frame)) {
		blog(LOG_WARNING, "[face swap: '%s'] frame preprocessing failed, passing through",
		     obs_source_get_name(filter->context));
		return;
	}

	/* Normalisation is detector-specific; see FACE_SWAP_DETECTORS. */
	if (!filter->detector_ready || !filter->detector_spec)
		return;

	if (!face_swap_rgb_to_letterbox_chw(filter->detect_tensor, filter->detect_tensor_count, filter->rgb, width,
					    height, FACE_SWAP_DETECT_SIZE, FACE_SWAP_DETECT_SIZE,
					    &filter->detector_spec->params, &filter->letterbox)) {
		blog(LOG_WARNING, "[face swap: '%s'] detection tensor preparation failed, passing through",
		     obs_source_get_name(filter->context));
		return;
	}

	{
		if (face_swap_ort_run_float(&filter->ort, &filter->detector, filter->detect_tensor,
					    filter->detect_tensor_count, filter->detector_spec->decode,
					    &filter->detection)) {
			if (filter->detection.score > 0.0f)
				face_swap_detection_unletterbox(&filter->detection, &filter->letterbox);
		} else if (!filter->warned_inference) {
			blog(LOG_WARNING, "[face swap: '%s'] face detection failed, passing through",
			     obs_source_get_name(filter->context));
			filter->warned_inference = true;
		}
	}

	face_swap_stabilizer_update(&filter->stabilizer, &filter->detection);

	if (!filter->style_ready || !filter->stabilizer.valid || filter->stabilizer.stable_frames < 2)
		return;
	if (!face_swap_estimate_alignment(&filter->stabilizer.value, &filter->affine))
		return;
	if (!face_swap_align_rgb(filter->face_crop, filter->rgb, width, height, &filter->affine))
		return;

	/* Image-to-image generators take pixel / 255 and emit [0, 1]. */
	static const struct face_swap_tensor_params style_params = {
		.mean = {0.0f, 0.0f, 0.0f},
		.scale = {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f},
		.bgr = false,
	};
	filter->styled_ready = face_swap_rgb_to_chw(filter->style_tensor, FACE_SWAP_STYLE_TENSOR, filter->face_crop,
						    FACE_SWAP_CROP_SIZE, FACE_SWAP_CROP_SIZE, FACE_SWAP_CROP_SIZE,
						    FACE_SWAP_CROP_SIZE, &style_params) &&
			       face_swap_ort_run_float(&filter->ort, &filter->style, filter->style_tensor,
						       FACE_SWAP_STYLE_TENSOR, face_swap_capture_swap,
						       filter->styled_crop);
}

static void *face_swap_worker(void *data)
{
	struct face_swap_data *filter = data;

	if (face_swap_ort_load(&filter->ort) && filter->ort.sdk_available) {
		blog(LOG_INFO, "[face swap: '%s'] ONNX Runtime %s loaded", obs_source_get_name(filter->context),
		     filter->ort.version);

		const int64_t detect_shape[4] = {1, 3, FACE_SWAP_DETECT_SIZE, FACE_SWAP_DETECT_SIZE};
		for (size_t i = 0; i < FACE_SWAP_DETECTOR_COUNT; i++) {
			const struct face_swap_detector_spec *spec = &FACE_SWAP_DETECTORS[i];
			if (!face_swap_open_session(&filter->ort, spec->file, detect_shape, &filter->detector))
				continue;
			filter->detector_spec = spec;
			filter->detector_ready = true;
			blog(LOG_INFO, "[face swap: '%s'] %s detector ready ('%s')",
			     obs_source_get_name(filter->context), spec->label, spec->file);
			break;
		}
		if (!filter->detector_ready)
			blog(LOG_WARNING,
			     "[face swap: '%s'] no detector found in the model directory, passing through",
			     obs_source_get_name(filter->context));
	} else {
		blog(LOG_WARNING, "[face swap: '%s'] ONNX Runtime is unavailable, passing through",
		     obs_source_get_name(filter->context));
	}

	for (;;) {
		int work_slot = -1;
		uint64_t work_sequence = 0;
		int detection_fps = 0;
		char *requested_style = NULL;

		os_event_wait(filter->wake_event);

		pthread_mutex_lock(&filter->mutex);
		if (filter->stop) {
			pthread_mutex_unlock(&filter->mutex);
			break;
		}

		const char *wanted = filter->style_model ? filter->style_model : "";
		const char *loaded = filter->loaded_style ? filter->loaded_style : "";
		if (strcmp(wanted, loaded) != 0)
			requested_style = bstrdup(wanted);

		const bool resize = filter->requested_width &&
				    (!filter->input[0].frame || filter->input[0].width != filter->requested_width ||
				     filter->input[0].height != filter->requested_height ||
				     filter->input[0].format != filter->requested_format);
		if (resize) {
			const bool first = face_swap_slot_prepare(&filter->input[0], filter->requested_format,
								 filter->requested_width, filter->requested_height);
			const bool second = face_swap_slot_prepare(&filter->input[1], filter->requested_format,
								  filter->requested_width, filter->requested_height);
			if (!first || !second) {
				face_swap_slot_reset(&filter->input[0]);
				face_swap_slot_reset(&filter->input[1]);
				blog(LOG_WARNING, "[face swap: '%s'] could not allocate input slots, passing through",
				     obs_source_get_name(filter->context));
			}
			filter->write_slot = 0;
			filter->published_slot = -1;
		}

		if (filter->published_slot >= 0) {
			work_slot = filter->published_slot;
			work_sequence = filter->input_sequence;
			detection_fps = filter->detection_fps;
			filter->published_slot = -1;
			filter->write_slot = 1 - work_slot;
		}
		pthread_mutex_unlock(&filter->mutex);

		if (requested_style)
			face_swap_reload_style(filter, requested_style);

		if (work_slot < 0)
			continue;

		const uint64_t timestamp = filter->input[work_slot].frame->timestamp;
		const uint64_t interval = detection_fps > 0 ? 1000000000ULL / detection_fps : 0;
		const bool throttled = interval && filter->last_inference_timestamp &&
				       timestamp > filter->last_inference_timestamp &&
				       timestamp - filter->last_inference_timestamp < interval;

		if (!throttled) {
			filter->last_inference_timestamp = timestamp;
			face_swap_run_inference(filter, work_slot);

			pthread_mutex_lock(&filter->mutex);
			filter->published_detection = filter->stabilizer.value;
			filter->published_opacity = filter->stabilizer.opacity;
			filter->published_timestamp = timestamp;
			filter->published_width = filter->input[work_slot].width;
			filter->published_height = filter->input[work_slot].height;
			filter->published_crop_ready = filter->styled_ready;
			if (filter->styled_ready) {
				memcpy(filter->published_crop, filter->styled_crop, sizeof(filter->published_crop));
				filter->published_affine = filter->affine;
			}
			pthread_mutex_unlock(&filter->mutex);
		}

		pthread_mutex_lock(&filter->mutex);
		if (work_sequence > filter->consumed_sequence)
			filter->consumed_sequence = work_sequence;
		pthread_mutex_unlock(&filter->mutex);
	}

	face_swap_ort_release_session(&filter->ort, &filter->detector);
	face_swap_ort_release_session(&filter->ort, &filter->style);
	face_swap_ort_unload(&filter->ort);
	bfree(filter->loaded_style);

	return NULL;
}

static void face_swap_update(void *data, obs_data_t *settings)
{
	struct face_swap_data *filter = data;
	char *style_model = bstrdup(obs_data_get_string(settings, S_STYLE_MODEL));

	pthread_mutex_lock(&filter->mutex);
	bfree(filter->style_model);
	filter->style_model = style_model;
	filter->strength = obs_data_get_double(settings, S_STRENGTH);
	filter->detection_fps = (int)obs_data_get_int(settings, S_DETECTION_FPS);
	filter->provider = (enum face_swap_provider)obs_data_get_int(settings, S_EXECUTION_PROVIDER);
	filter->debug_overlay = obs_data_get_bool(settings, S_DEBUG_OVERLAY);
	pthread_mutex_unlock(&filter->mutex);

	os_event_signal(filter->wake_event);
}

static void *face_swap_create(obs_data_t *settings, obs_source_t *context)
{
	struct face_swap_data *filter = bzalloc(sizeof(*filter));
	filter->context = context;
	filter->published_slot = -1;

	pthread_mutex_init(&filter->mutex, NULL);
	if (os_event_init(&filter->wake_event, OS_EVENT_TYPE_AUTO) != 0)
		goto fail;

	face_swap_update(filter, settings);
	if (pthread_create(&filter->worker, NULL, face_swap_worker, filter) != 0)
		goto fail;
	filter->worker_started = true;
	return filter;

fail:
	if (filter->wake_event)
		os_event_destroy(filter->wake_event);
	bfree(filter->style_model);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
	return NULL;
}

static void face_swap_destroy(void *data)
{
	struct face_swap_data *filter = data;

	if (!filter)
		return;

	pthread_mutex_lock(&filter->mutex);
	filter->stop = true;
	pthread_mutex_unlock(&filter->mutex);

	if (filter->worker_started) {
		os_event_signal(filter->wake_event);
		pthread_join(filter->worker, NULL);
	}

	face_swap_slot_reset(&filter->input[0]);
	face_swap_slot_reset(&filter->input[1]);
	bfree(filter->rgb);
	bfree(filter->detect_tensor);
	os_event_destroy(filter->wake_event);
	bfree(filter->style_model);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
}

static struct obs_source_frame *face_swap_video(void *data, struct obs_source_frame *frame)
{
	struct face_swap_data *filter = data;
	struct face_swap_detection detection = {0};
	struct face_swap_affine affine = {0};
	float opacity = 0.0f;
	float strength = 0.0f;
	bool debug_overlay = false;
	bool result_matches = false;
	bool crop_ready = false;

	if (!face_swap_supported_format(frame->format)) {
		if (!filter->warned_unsupported_format) {
			blog(LOG_WARNING, "[face swap: '%s'] unsupported format %d, passing through",
			     obs_source_get_name(filter->context), (int)frame->format);
			filter->warned_unsupported_format = true;
		}
		return frame;
	}

	/* Never wait behind the worker or allocate on the video path. */
	if (pthread_mutex_trylock(&filter->mutex) != 0)
		return frame;

	if (!face_swap_slot_matches(&filter->input[filter->write_slot], frame)) {
		filter->requested_width = frame->width;
		filter->requested_height = frame->height;
		filter->requested_format = frame->format;
	} else {
		obs_source_frame_copy(filter->input[filter->write_slot].frame, frame);
		filter->input[filter->write_slot].published = true;
		filter->published_slot = filter->write_slot;
		filter->input_sequence++;
	}
	os_event_signal(filter->wake_event);

	detection = filter->published_detection;
	opacity = filter->published_opacity;
	strength = (float)filter->strength;
	debug_overlay = filter->debug_overlay;
	result_matches = filter->published_width == frame->width && filter->published_height == frame->height &&
			 frame->timestamp >= filter->published_timestamp &&
			 frame->timestamp - filter->published_timestamp <= 1000000000ULL;
	crop_ready = result_matches && filter->published_crop_ready;
	if (crop_ready) {
		memcpy(filter->video_crop, filter->published_crop, sizeof(filter->video_crop));
		affine = filter->published_affine;
	}
	pthread_mutex_unlock(&filter->mutex);

	/* Only the face mask is rewritten, so the background always comes from
	 * this frame even though inference ran on an older one. */
	if (crop_ready && opacity > 0.0f)
		face_swap_blend_crop_frame(frame, filter->video_crop, &affine, strength, opacity);

	if (debug_overlay && result_matches && opacity > 0.0f)
		face_swap_draw_detection(frame, &detection, opacity);

	/* Without a result and with the overlay disabled, this is exact passthrough. */
	return frame;
}

/* Lists every .onnx in the model directory except the detector itself. */
static void face_swap_fill_style_list(obs_property_t *list)
{
	obs_property_list_add_string(list, obs_module_text("FaceSwap.NoStyle"), "");

	char *dir_path = os_get_config_path_ptr(FACE_SWAP_MODEL_DIR);
	if (!dir_path)
		return;

	os_dir_t *dir = os_opendir(dir_path);
	if (dir) {
		struct os_dirent *entry;
		while ((entry = os_readdir(dir)) != NULL) {
			if (entry->directory)
				continue;
			const size_t len = strlen(entry->d_name);
			if (len < 6 || astrcmpi(entry->d_name + len - 5, ".onnx") != 0)
				continue;

			bool is_detector = false;
			for (size_t i = 0; i < FACE_SWAP_DETECTOR_COUNT; i++)
				is_detector = is_detector || astrcmpi(entry->d_name, FACE_SWAP_DETECTORS[i].file) == 0;
			if (is_detector)
				continue;

			obs_property_list_add_string(list, entry->d_name, entry->d_name);
		}
		os_closedir(dir);
	}

	bfree(dir_path);
}

static obs_properties_t *face_swap_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "model_info", obs_module_text("FaceSwap.ModelInfo"), OBS_TEXT_INFO);

	obs_property_t *style = obs_properties_add_list(props, S_STYLE_MODEL, obs_module_text("FaceSwap.StyleModel"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	face_swap_fill_style_list(style);

	obs_properties_add_float_slider(props, S_STRENGTH, obs_module_text("FaceSwap.Strength"), 0.0, 1.0, 0.01);

	obs_property_t *fps = obs_properties_add_list(props, S_DETECTION_FPS, obs_module_text("FaceSwap.DetectionFps"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "10", 10);
	obs_property_list_add_int(fps, "15", 15);
	obs_property_list_add_int(fps, "30", 30);

	obs_property_t *provider = obs_properties_add_list(props, S_EXECUTION_PROVIDER,
							   obs_module_text("FaceSwap.Provider"), OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(provider, obs_module_text("FaceSwap.Auto"), FACE_SWAP_PROVIDER_AUTO);
	obs_property_list_add_int(provider, "CUDA", FACE_SWAP_PROVIDER_CUDA);
	obs_property_list_add_int(provider, "DirectML", FACE_SWAP_PROVIDER_DIRECTML);
	obs_property_list_add_int(provider, "CPU", FACE_SWAP_PROVIDER_CPU);

	obs_properties_add_bool(props, S_DEBUG_OVERLAY, obs_module_text("FaceSwap.DebugOverlay"));

	UNUSED_PARAMETER(data);
	return props;
}

static void face_swap_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_STYLE_MODEL, "");
	obs_data_set_default_double(settings, S_STRENGTH, 1.0);
	obs_data_set_default_int(settings, S_DETECTION_FPS, 15);
	obs_data_set_default_int(settings, S_EXECUTION_PROVIDER, FACE_SWAP_PROVIDER_AUTO);
	obs_data_set_default_bool(settings, S_DEBUG_OVERLAY, false);
}

struct obs_source_info face_swap_filter = {
	.id = "face_swap_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = face_swap_name,
	.create = face_swap_create,
	.destroy = face_swap_destroy,
	.update = face_swap_update,
	.get_properties = face_swap_properties,
	.get_defaults = face_swap_defaults,
	.filter_video = face_swap_video,
};
