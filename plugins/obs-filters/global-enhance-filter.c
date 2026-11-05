/*
 * PPCDN global picture enhancement filter (GPU, scene-safe).
 *
 * One render pass combines four operations on the fully composited scene:
 * motion-adaptive temporal denoise, skin-selective basic beauty, local
 * contrast (clarity), and vibrance.  A ping-pong texture keeps the previous
 * enhanced frame for denoising; the filter remains zero-latency (it never
 * queues or delays a frame).
 *
 * The beauty stage intentionally stays "basic": skin-tone-selective
 * smoothing plus a small rosy lift.  Geometry-changing effects such as eye
 * enlargement and chin warping require reliable face landmarks and belong
 * in the existing camera-only beauty_filter, not in a scene-wide filter.
 */

#include <obs-module.h>

#define S_TEMPORAL "temporal_strength"
#define S_BEAUTY "beauty_strength"
#define S_CLARITY "clarity_strength"
#define S_VIVIDNESS "vividness_strength"

struct global_enhance_data {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_texrender_t *frames[2];

	gs_eparam_t *previous_image;
	gs_eparam_t *history_valid_param;
	gs_eparam_t *temporal_param;
	gs_eparam_t *beauty_param;
	gs_eparam_t *clarity_param;
	gs_eparam_t *vividness_param;
	gs_eparam_t *texture_width;
	gs_eparam_t *texture_height;

	float temporal;
	float beauty;
	float clarity;
	float vividness;

	uint32_t width;
	uint32_t height;
	enum gs_color_space space;
	enum gs_color_format format;
	int current_frame;
	bool history_valid;
	bool processed_frame;
};

static const char *global_enhance_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GlobalEnhanceFilter");
}

static void global_enhance_reset(struct global_enhance_data *filter)
{
	filter->history_valid = false;
	filter->processed_frame = false;
	filter->current_frame = 0;

	for (size_t i = 0; i < OBS_COUNTOF(filter->frames); i++) {
		if (filter->frames[i])
			gs_texrender_reset(filter->frames[i]);
	}
}

static void global_enhance_update(void *data, obs_data_t *settings)
{
	struct global_enhance_data *filter = data;
	filter->temporal = (float)obs_data_get_double(settings, S_TEMPORAL);
	filter->beauty = (float)obs_data_get_double(settings, S_BEAUTY);
	filter->clarity = (float)obs_data_get_double(settings, S_CLARITY);
	filter->vividness = (float)obs_data_get_double(settings, S_VIVIDNESS);

	/* Do not blend a frame processed with old settings into the new look. */
	filter->history_valid = false;
}

static void global_enhance_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_TEMPORAL, 0.30);
	obs_data_set_default_double(settings, S_BEAUTY, 0.25);
	obs_data_set_default_double(settings, S_CLARITY, 0.35);
	obs_data_set_default_double(settings, S_VIVIDNESS, 0.30);
}

static obs_properties_t *global_enhance_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "sdr_only_info", obs_module_text("GlobalEnhance.SdrOnlyInfo"),
				OBS_TEXT_INFO);
	obs_properties_add_float_slider(props, S_TEMPORAL, obs_module_text("GlobalEnhance.Temporal"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(props, S_BEAUTY, obs_module_text("GlobalEnhance.Beauty"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(props, S_CLARITY, obs_module_text("GlobalEnhance.Clarity"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(props, S_VIVIDNESS, obs_module_text("GlobalEnhance.Vividness"), 0.0, 1.0,
					0.01);

	UNUSED_PARAMETER(data);
	return props;
}

static void *global_enhance_create(obs_data_t *settings, obs_source_t *context)
{
	struct global_enhance_data *filter = bzalloc(sizeof(*filter));
	char *effect_path = obs_module_file("global-enhance.effect");

	filter->context = context;
	filter->space = GS_CS_SRGB;
	filter->format = GS_RGBA;

	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	filter->frames[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->frames[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(effect_path);

	if (!filter->effect || !filter->frames[0] || !filter->frames[1]) {
		blog(LOG_ERROR, "Global enhance: failed to create GPU resources");
		if (filter->effect || filter->frames[0] || filter->frames[1]) {
			obs_enter_graphics();
			gs_effect_destroy(filter->effect);
			gs_texrender_destroy(filter->frames[0]);
			gs_texrender_destroy(filter->frames[1]);
			obs_leave_graphics();
		}
		bfree(filter);
		return NULL;
	}

	filter->previous_image = gs_effect_get_param_by_name(filter->effect, "previous_image");
	filter->history_valid_param = gs_effect_get_param_by_name(filter->effect, "history_valid");
	filter->temporal_param = gs_effect_get_param_by_name(filter->effect, S_TEMPORAL);
	filter->beauty_param = gs_effect_get_param_by_name(filter->effect, S_BEAUTY);
	filter->clarity_param = gs_effect_get_param_by_name(filter->effect, S_CLARITY);
	filter->vividness_param = gs_effect_get_param_by_name(filter->effect, S_VIVIDNESS);
	filter->texture_width = gs_effect_get_param_by_name(filter->effect, "texture_width");
	filter->texture_height = gs_effect_get_param_by_name(filter->effect, "texture_height");

	global_enhance_update(filter, settings);
	return filter;
}

static void global_enhance_destroy(void *data)
{
	struct global_enhance_data *filter = data;

	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	gs_texrender_destroy(filter->frames[0]);
	gs_texrender_destroy(filter->frames[1]);
	obs_leave_graphics();

	bfree(filter);
}

static void global_enhance_tick(void *data, float seconds)
{
	struct global_enhance_data *filter = data;
	filter->processed_frame = false;
	UNUSED_PARAMETER(seconds);
}

static bool global_enhance_prepare(struct global_enhance_data *filter, obs_source_t *target)
{
	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const uint32_t width = obs_source_get_width(target);
	const uint32_t height = obs_source_get_height(target);
	if (!width || !height)
		return false;

	const enum gs_color_space space =
		obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);

	if (width != filter->width || height != filter->height || space != filter->space || format != filter->format) {
		filter->width = width;
		filter->height = height;
		filter->space = space;
		filter->format = format;

		for (size_t i = 0; i < OBS_COUNTOF(filter->frames); i++) {
			if (gs_texrender_get_format(filter->frames[i]) != format) {
				gs_texrender_destroy(filter->frames[i]);
				filter->frames[i] = gs_texrender_create(format, GS_ZS_NONE);
			} else {
				gs_texrender_reset(filter->frames[i]);
			}
		}

		global_enhance_reset(filter);
	}

	return filter->frames[0] && filter->frames[1];
}

static void global_enhance_draw(struct global_enhance_data *filter)
{
	gs_texture_t *texture = gs_texrender_get_texture(filter->frames[filter->current_frame]);
	if (!texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	const bool previous_srgb = gs_framebuffer_srgb_enabled();

	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(image, texture);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, filter->width, filter->height);
	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous_srgb);
}

static void global_enhance_render(void *data, gs_effect_t *unused)
{
	struct global_enhance_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);

	UNUSED_PARAMETER(unused);

	if (!target || !global_enhance_prepare(filter, target)) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	/* The shader's luma ranges and skin thresholds are defined in SDR. */
	if (filter->space == GS_CS_709_EXTENDED) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (filter->processed_frame) {
		global_enhance_draw(filter);
		return;
	}

	if (!obs_source_process_filter_begin_with_color_space(filter->context, filter->format, filter->space,
							      OBS_ALLOW_DIRECT_RENDERING))
		return;

	const int write_frame = filter->history_valid ? 1 - filter->current_frame : 0;
	gs_texrender_t *destination = filter->frames[write_frame];
	gs_texrender_reset(destination);

	gs_texture_t *previous =
		filter->history_valid ? gs_texrender_get_texture(filter->frames[filter->current_frame]) : NULL;
	gs_effect_set_texture_srgb(filter->previous_image, previous);
	gs_effect_set_float(filter->history_valid_param, filter->history_valid ? 1.0f : 0.0f);
	gs_effect_set_float(filter->temporal_param, filter->temporal);
	gs_effect_set_float(filter->beauty_param, filter->beauty);
	gs_effect_set_float(filter->clarity_param, filter->clarity);
	gs_effect_set_float(filter->vividness_param, filter->vividness);
	gs_effect_set_float(filter->texture_width, (float)filter->width);
	gs_effect_set_float(filter->texture_height, (float)filter->height);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool rendered = false;
	if (gs_texrender_begin_with_color_space(destination, filter->width, filter->height, filter->space)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)filter->width, 0.0f, (float)filter->height, -100.0f, 100.0f);
		obs_source_process_filter_end(filter->context, filter->effect, filter->width, filter->height);
		gs_texrender_end(destination);
		rendered = true;
	} else {
		/* Complete the begin/end pair and pass through this frame. */
		obs_source_process_filter_end(filter->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), filter->width,
						     filter->height);
	}

	gs_blend_state_pop();

	if (!rendered)
		return;

	filter->current_frame = write_frame;
	filter->history_valid = true;
	filter->processed_frame = true;
	global_enhance_draw(filter);
}

static enum gs_color_space global_enhance_get_color_space(void *data, size_t count,
							  const enum gs_color_space *preferred_spaces)
{
	struct global_enhance_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	if (!target)
		return GS_CS_SRGB;
	return obs_source_get_color_space(target, count, preferred_spaces);
}

struct obs_source_info global_enhance_filter = {
	.id = "global_enhance_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = global_enhance_name,
	.create = global_enhance_create,
	.destroy = global_enhance_destroy,
	.update = global_enhance_update,
	.get_defaults = global_enhance_defaults,
	.get_properties = global_enhance_properties,
	.video_tick = global_enhance_tick,
	.video_render = global_enhance_render,
	.video_get_color_space = global_enhance_get_color_space,
};
