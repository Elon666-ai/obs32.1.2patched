/*
 * Clarity filter: GPU local-contrast ("clarity") enhancement.
 *
 * Unlike the sharpen filter, which boosts pixel-scale edges with a 3x3
 * kernel, this boosts mid-frequency local contrast over a ~7px (at
 * 1080p, resolution-scaled) radius, which reads as "haze removal" /
 * added punch rather than edge crispening. The boost is mid-tone
 * weighted and soft-limited in the shader, so shadows/highlights and
 * strong edges are protected. See clarity.effect for the algorithm.
 */

#include <obs-module.h>

struct clarity_data {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *strength_param;
	gs_eparam_t *texture_width, *texture_height;

	float strength;
};

static const char *clarity_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ClarityFilter");
}

static void clarity_update(void *data, obs_data_t *settings)
{
	struct clarity_data *filter = data;

	filter->strength = (float)obs_data_get_double(settings, "strength");
}

static void clarity_destroy(void *data)
{
	struct clarity_data *filter = data;

	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
	}

	bfree(data);
}

static void *clarity_create(obs_data_t *settings, obs_source_t *context)
{
	struct clarity_data *filter = bzalloc(sizeof(struct clarity_data));
	char *effect_path = obs_module_file("clarity.effect");

	filter->context = context;

	obs_enter_graphics();

	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	if (filter->effect) {
		filter->strength_param = gs_effect_get_param_by_name(filter->effect, "strength");
		filter->texture_width = gs_effect_get_param_by_name(filter->effect, "texture_width");
		filter->texture_height = gs_effect_get_param_by_name(filter->effect, "texture_height");
	}

	obs_leave_graphics();

	bfree(effect_path);

	if (!filter->effect) {
		clarity_destroy(filter);
		return NULL;
	}

	clarity_update(filter, settings);
	return filter;
}

static void clarity_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct clarity_data *filter = data;

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	/* The mid-tone weighting assumes SDR-range luma; on HDR content
	 * it would misclassify everything as highlights, so skip. */
	const enum gs_color_space source_space = obs_source_get_color_space(
		obs_filter_get_target(filter->context), OBS_COUNTOF(preferred_spaces), preferred_spaces);
	if (source_space == GS_CS_709_EXTENDED) {
		obs_source_skip_video_filter(filter->context);
	} else {
		const enum gs_color_format format = gs_get_format_from_space(source_space);
		if (obs_source_process_filter_begin_with_color_space(filter->context, format, source_space,
								     OBS_ALLOW_DIRECT_RENDERING)) {
			obs_source_t *target = obs_filter_get_target(filter->context);

			gs_effect_set_float(filter->strength_param, filter->strength);
			gs_effect_set_float(filter->texture_width, (float)obs_source_get_width(target));
			gs_effect_set_float(filter->texture_height, (float)obs_source_get_height(target));

			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

			obs_source_process_filter_end(filter->context, filter->effect, 0, 0);

			gs_blend_state_pop();
		}
	}
}

static obs_properties_t *clarity_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "sdr_only_info", obs_module_text("SdrOnlyInfo"), OBS_TEXT_INFO);
	obs_properties_add_float_slider(props, "strength", obs_module_text("Clarity.Strength"), 0.0, 1.0, 0.01);

	UNUSED_PARAMETER(data);
	return props;
}

static void clarity_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 0.5);
}

static enum gs_color_space clarity_get_color_space(void *data, size_t count,
						   const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	struct clarity_data *const filter = data;
	return obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(potential_spaces),
					  potential_spaces);
}

struct obs_source_info clarity_filter = {
	.id = "clarity_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = clarity_getname,
	.create = clarity_create,
	.destroy = clarity_destroy,
	.update = clarity_update,
	.video_render = clarity_render,
	.get_properties = clarity_properties,
	.get_defaults = clarity_defaults,
	.video_get_color_space = clarity_get_color_space,
};
