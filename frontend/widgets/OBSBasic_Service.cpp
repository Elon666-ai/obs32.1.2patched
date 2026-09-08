/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <cmath>

constexpr std::string_view OBSServiceFileName = "service.json";

void OBSBasic::SaveService()
{
	if (!service)
		return;

	const OBSProfile &currentProfile = GetCurrentProfile();

	const std::filesystem::path jsonFilePath = currentProfile.path / std::filesystem::u8path(OBSServiceFileName);

	OBSDataAutoRelease data = obs_data_create();
	OBSDataAutoRelease settings = obs_service_get_settings(service);

	obs_data_set_string(data, "type", obs_service_get_type(service));
	obs_data_set_obj(data, "settings", settings);

	if (!obs_data_save_json_safe(data, jsonFilePath.u8string().c_str(), "tmp", "bak")) {
		blog(LOG_WARNING, "Failed to save service");
	}
}

bool OBSBasic::LoadService()
{
	OBSDataAutoRelease data;

	try {
		const OBSProfile &currentProfile = GetCurrentProfile();

		const std::filesystem::path jsonFilePath =
			currentProfile.path / std::filesystem::u8path(OBSServiceFileName);

		data = obs_data_create_from_json_file_safe(jsonFilePath.u8string().c_str(), "bak");

		if (!data) {
			return false;
		}
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
		return false;
	}

	const char *type;
	obs_data_set_default_string(data, "type", "rtmp_common");
	type = obs_data_get_string(data, "type");

	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	OBSDataAutoRelease hotkey_data = obs_data_get_obj(data, "hotkeys");

	service = obs_service_create(type, "default_service", settings, hotkey_data);
	obs_service_release(service);

	if (!service)
		return false;

	/* Enforce Opus on WHIP if needed */
	if (strcmp(obs_service_get_protocol(service), "WHIP") == 0) {
		const char *option = config_get_string(activeConfiguration, "SimpleOutput", "StreamAudioEncoder");
		if (strcmp(option, "opus") != 0)
			config_set_string(activeConfiguration, "SimpleOutput", "StreamAudioEncoder", "opus");

		option = config_get_string(activeConfiguration, "AdvOut", "AudioEncoder");

		const char *encoder_codec = obs_get_encoder_codec(option);
		if (!encoder_codec || strcmp(encoder_codec, "opus") != 0)
			config_set_string(activeConfiguration, "AdvOut", "AudioEncoder", "ffmpeg_opus");
	}

	// One-time migration: Manual ROI used to live on the WHIP service's
	// settings; it's now a global "Video" config setting independent of
	// which service/protocol is selected (see WHIPOutput::ApplyRoi() and
	// OBSBasicSettings::LoadVideoSettings()). Only migrate off a WHIP
	// service (the only place these keys were ever written), and only if
	// nothing has been saved under the new keys yet.
	if (strcmp(obs_service_get_type(service), "whip_custom") == 0 &&
	    !config_has_user_value(activeConfiguration, "Video", "RoiEnabled")) {
		OBSDataAutoRelease serviceSettings = obs_service_get_settings(service);
		if (obs_data_has_user_value(serviceSettings, "roi_left") ||
		    obs_data_has_user_value(serviceSettings, "roi_enabled")) {
			config_set_bool(activeConfiguration, "Video", "RoiEnabled",
					obs_data_get_bool(serviceSettings, "roi_enabled"));
			config_set_int(activeConfiguration, "Video", "RoiLeft",
				       obs_data_get_int(serviceSettings, "roi_left"));
			config_set_int(activeConfiguration, "Video", "RoiTop",
				       obs_data_get_int(serviceSettings, "roi_top"));
			config_set_int(activeConfiguration, "Video", "RoiRight",
				       obs_data_get_int(serviceSettings, "roi_right"));
			config_set_int(activeConfiguration, "Video", "RoiBottom",
				       obs_data_get_int(serviceSettings, "roi_bottom"));
			if (obs_data_has_user_value(serviceSettings, "roi_priority"))
				config_set_int(activeConfiguration, "Video", "RoiPriority",
					       (int)std::lround(obs_data_get_double(serviceSettings, "roi_priority") *
								 51.0));
			if (obs_data_has_user_value(serviceSettings, "roi_bg_priority"))
				config_set_int(
					activeConfiguration, "Video", "RoiBgPriority",
					(int)std::lround(obs_data_get_double(serviceSettings, "roi_bg_priority") *
							  -51.0));
			activeConfiguration.SaveSafe("tmp");
		}
	}

	return true;
}

bool OBSBasic::InitService()
{
	ProfileScope("OBSBasic::InitService");

	if (LoadService())
		return true;

	service = obs_service_create("rtmp_common", "default_service", nullptr, nullptr);
	if (!service)
		return false;
	obs_service_release(service);

	return true;
}

obs_service_t *OBSBasic::GetService()
{
	if (!service) {
		service = obs_service_create("rtmp_common", NULL, NULL, nullptr);
		obs_service_release(service);
	}
	return service;
}

void OBSBasic::SetService(obs_service_t *newService)
{
	if (newService) {
		service = newService;
	}
}
