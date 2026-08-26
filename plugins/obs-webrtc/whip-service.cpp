#include "whip-service.h"

const char *audio_codecs[] = {"opus", nullptr};
const char *video_codecs[] = {"h264", "hevc", "av1", nullptr};

WHIPService::WHIPService(obs_data_t *settings, obs_service_t *) : server(), backup_server(), bearer_token()
{
	Update(settings);
}

void WHIPService::Update(obs_data_t *settings)
{
	server = obs_data_get_string(settings, "server");
	backup_server = obs_data_get_string(settings, "backup_server");
	bearer_token = obs_data_get_string(settings, "bearer_token");
}

obs_properties_t *WHIPService::Properties()
{
	obs_properties_t *ppts = obs_properties_create();

	obs_properties_add_text(ppts, "server", "URL", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "backup_server", obs_module_text("Service.BackupServer"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "bearer_token", obs_module_text("Service.BearerToken"), OBS_TEXT_PASSWORD);

	// Encoder ROI: prioritize quality inside a rectangle (at the output
	// resolution) and optionally degrade everything outside it. Applied
	// per simulcast layer by WHIPOutput on stream start.
	obs_properties_t *roi = obs_properties_create();
	obs_properties_add_int(roi, "roi_left", obs_module_text("Service.RoiLeft"), 0, 16384, 2);
	obs_properties_add_int(roi, "roi_top", obs_module_text("Service.RoiTop"), 0, 16384, 2);
	obs_properties_add_int(roi, "roi_right", obs_module_text("Service.RoiRight"), 0, 16384, 2);
	obs_properties_add_int(roi, "roi_bottom", obs_module_text("Service.RoiBottom"), 0, 16384, 2);
	obs_properties_add_float_slider(roi, "roi_priority", obs_module_text("Service.RoiPriority"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(roi, "roi_bg_priority", obs_module_text("Service.RoiBgPriority"), -1.0, 0.0,
					0.05);
	obs_properties_add_group(ppts, "roi_enabled", obs_module_text("Service.Roi"), OBS_GROUP_CHECKABLE, roi);

	return ppts;
}

void WHIPService::Defaults(obs_data_t *defaults)
{
	// Master switch for detection-driven ROI (balls + people), set from
	// Settings > Stream > Advanced Options; default on.
	obs_data_set_default_bool(defaults, "detect_roi", true);
	// Full-reference quality score of the outgoing stream (program feed
	// = 100), same settings page; default on.
	obs_data_set_default_bool(defaults, "quality_score", true);
	obs_data_set_default_bool(defaults, "roi_enabled", false);
	obs_data_set_default_int(defaults, "roi_left", 0);
	obs_data_set_default_int(defaults, "roi_top", 0);
	obs_data_set_default_int(defaults, "roi_right", 0);
	obs_data_set_default_int(defaults, "roi_bottom", 0);
	// +-3 QP (see WHIPOutput::ApplyRoi()'s matching re-seed and
	// OBSBasicSettings::LoadStream1Settings()'s QP<->priority
	// conversion) - roughly "inside looks 2x the quality of outside"
	// under the ~6-QP-per-2x-bitrate rule of thumb.
	obs_data_set_default_double(defaults, "roi_priority", 3.0 / 51.0);
	obs_data_set_default_double(defaults, "roi_bg_priority", -3.0 / 51.0);
}

void WHIPService::ApplyEncoderSettings(obs_data_t *video_settings, obs_data_t *)
{
	// For now, ensure maximum compatibility with webrtc peers
	if (video_settings) {
		obs_data_set_int(video_settings, "bf", 0);
		obs_data_set_bool(video_settings, "repeat_headers", true);
	}
}

const char *WHIPService::GetConnectInfo(enum obs_service_connect_info type)
{
	switch (type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return server.c_str();
	case OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN:
		return bearer_token.c_str();
	case OBS_SERVICE_CONNECT_INFO_BACKUP_SERVER:
		return backup_server.c_str();
	default:
		return nullptr;
	}
}

bool WHIPService::CanTryToConnect()
{
	return !server.empty();
}

const char *WHIPService::GetBackupServer() const
{
	return backup_server.c_str();
}

void register_whip_service()
{
	struct obs_service_info info = {};

	info.id = "whip_custom";
	info.get_name = [](void *) -> const char * {
		return obs_module_text("Service.Name");
	};
	info.create = [](obs_data_t *settings, obs_service_t *service) -> void * {
		return new WHIPService(settings, service);
	};
	info.destroy = [](void *priv_data) {
		delete static_cast<WHIPService *>(priv_data);
	};
	info.update = [](void *priv_data, obs_data_t *settings) {
		static_cast<WHIPService *>(priv_data)->Update(settings);
	};
	info.get_properties = [](void *) -> obs_properties_t * {
		return WHIPService::Properties();
	};
	info.get_defaults = [](obs_data_t *defaults) {
		WHIPService::Defaults(defaults);
	};
	info.get_protocol = [](void *) -> const char * {
		return "WHIP";
	};
	info.get_url = [](void *priv_data) -> const char * {
		return static_cast<WHIPService *>(priv_data)->server.c_str();
	};
	info.get_output_type = [](void *) -> const char * {
		return "whip_output";
	};
	info.apply_encoder_settings = [](void *, obs_data_t *video_settings, obs_data_t *audio_settings) {
		WHIPService::ApplyEncoderSettings(video_settings, audio_settings);
	};
	info.get_supported_video_codecs = [](void *) -> const char ** {
		return video_codecs;
	};
	info.get_supported_audio_codecs = [](void *) -> const char ** {
		return audio_codecs;
	};
	info.can_try_to_connect = [](void *priv_data) -> bool {
		return static_cast<WHIPService *>(priv_data)->CanTryToConnect();
	};
	info.get_connect_info = [](void *priv_data, uint32_t type) -> const char * {
		return static_cast<WHIPService *>(priv_data)->GetConnectInfo((enum obs_service_connect_info)type);
	};
	obs_register_service(&info);
}
