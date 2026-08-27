/******************************************************************************
    Copyright (C) 2025 by Sean DuBois <sean@pion.ly>

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
#pragma once

#include <string>

// One scaled-down Simulcast layer's target resolution/bitrate, as
// configured by the user in Settings > Stream > Simulcast (see
// OBSBasicSettings::RebuildWHIPSimulcastLayerRows in
// OBSBasicSettings_Stream.cpp, which builds those per-layer fields and
// saves them under the config keys read here). rid=0 (the highest layer)
// is never covered by this - it's always the main Stream encoder's own,
// unscaled output.
struct WHIPSimulcastLayer {
	// If true, width/height/bitrateKbps below are all meaningless - the
	// caller should recompute every one of them live against the main
	// output's *current* resolution/bitrate instead (see
	// WHIPSimulcastEncoders::Create()'s fallback formula and
	// WHIPSimulcastProportionalBitrate()), so this layer stays in
	// proportion to layer 0 even after layer 0's resolution/bitrate
	// changes (see the "WHIPSimulcastLayer<N>FollowMain" config key,
	// which defaults to true when absent - see the Update()/Create()
	// call sites below for why absence must mean "follow"). Width/
	// height/bitrateKbps are only ever saved (and only ever meaningful)
	// once the user has explicitly unchecked Follow Main for this row -
	// see SaveStream1Settings() in OBSBasicSettings_Stream.cpp.
	bool followMain;
	uint32_t width;
	uint32_t height;
	int bitrateKbps;
};

// Reads layer N's (N = 1, 2, ...; rid=0 excluded, see WHIPSimulcastLayer's
// comment) saved resolution/bitrate from config's "Stream1" section, using
// the same "WHIPSimulcastLayer<N>Width/Height/BitrateKbps" keys
// OBSBasicSettings_Stream.cpp's SaveStream1Settings() writes. Returns
// false if config has no user-saved bitrate value for that layer yet
// (e.g. this row was never touched, or config predates this per-layer
// UI), leaving *layer unmodified so the caller can fall back to the
// halve-per-slot formula for it entirely (both resolution and bitrate).
// Scales mainBitrate down by this layer's actual pixel-area ratio against
// the main output's resolution (width*height / mainWidth*mainHeight) - so a
// layer at 50%/50% of the main resolution (25% of its pixel count) gets 25%
// of its bitrate, whatever the resolution ladder used to arrive at that
// layer's width/height actually is (currently an even step-down, not a
// per-layer halving - see WHIPSimulcastEncoders::Create()). Clamped to at
// least 1 Kbps since most encoders reject/misbehave on a bitrate of 0.
static inline int64_t WHIPSimulcastProportionalBitrate(int mainBitrate, uint32_t mainWidth, uint32_t mainHeight,
							uint32_t layerWidth, uint32_t layerHeight)
{
	if (mainWidth == 0 || mainHeight == 0)
		return mainBitrate > 0 ? mainBitrate : 1;

	int64_t bitrate = (int64_t)mainBitrate * layerWidth * layerHeight / ((int64_t)mainWidth * mainHeight);
	return bitrate > 0 ? bitrate : 1;
}

static inline bool GetWHIPSimulcastLayerConfig(config_t *config, int layerNum, WHIPSimulcastLayer *layer)
{
	std::string prefix = "WHIPSimulcastLayer" + std::to_string(layerNum);
	std::string widthKey = prefix + "Width";
	std::string heightKey = prefix + "Height";
	std::string bitrateKey = prefix + "BitrateKbps";
	std::string followMainKey = prefix + "FollowMain";

	if (!config || !config_has_user_value(config, "Stream1", bitrateKey.c_str()))
		return false;

	// Absence of a saved FollowMain value means either a config from
	// before this option existed, or a row that's never been unchecked
	// - both should keep following the main output's resolution.
	layer->followMain = !config_has_user_value(config, "Stream1", followMainKey.c_str()) ||
			    config_get_bool(config, "Stream1", followMainKey.c_str());
	layer->width = static_cast<uint32_t>(config_get_int(config, "Stream1", widthKey.c_str()));
	layer->height = static_cast<uint32_t>(config_get_int(config, "Stream1", heightKey.c_str()));
	layer->bitrateKbps = static_cast<int>(config_get_int(config, "Stream1", bitrateKey.c_str()));

	if (layer->bitrateKbps <= 0)
		return false;
	if (!layer->followMain && (layer->width == 0 || layer->height == 0))
		return false;
	return true;
}

struct WHIPSimulcastEncoders {
public:
	// config is main->Config() (see AdvancedOutput.cpp/SimpleOutput.cpp
	// call sites) - read here rather than threaded through as
	// individual values so each layer can independently fall back to
	// the halve-per-slot formula if it has no saved config value (a mix
	// of user-customized and default layers is expected, e.g. right
	// after the user increases Total Layers and hasn't touched the new
	// row yet).
	void Create(const char *encoderId, obs_data_t *videoSettings, int rescaleFilter, int whipSimulcastTotalLayers,
		    uint32_t outputWidth, uint32_t outputHeight, config_t *config = nullptr)
	{
		if (rescaleFilter == OBS_SCALE_DISABLE) {
			rescaleFilter = OBS_SCALE_BICUBIC;
		}

		if (whipSimulcastTotalLayers <= 1) {
			return;
		}

		usingCustomLayer.assign(whipSimulcastTotalLayers, false);

		auto widthStep = outputWidth / whipSimulcastTotalLayers;
		auto heightStep = outputHeight / whipSimulcastTotalLayers;
		int videoBitrate = videoSettings ? static_cast<int>(obs_data_get_int(videoSettings, "bitrate")) : 0;
		std::string encoder_name = "whip_simulcast_0";

		for (auto i = whipSimulcastTotalLayers - 1; i > 0; i--) {
			uint32_t width;
			uint32_t height;

			encoder_name[encoder_name.size() - 1] = std::to_string(i).at(0);
			OBSDataAutoRelease layerSettings;

			// layer slot numbering (1, 2, ...) matches rid, which
			// counts down as i does here - see the Settings UI's
			// RebuildWHIPSimulcastLayerRows for the same
			// convention on the writing side.
			int slot = whipSimulcastTotalLayers - i;
			WHIPSimulcastLayer custom;
			bool haveCustom = GetWHIPSimulcastLayerConfig(config, slot, &custom);

			// Resolution: follow the main output's current size
			// (recomputed here from outputWidth/outputHeight,
			// which the caller re-reads via video_output_get_*
			// every time Create() runs after a ResetOutputs()) -
			// unless the user explicitly unchecked "Follow Main"
			// for this layer and locked in their own width/height.
			if (haveCustom && !custom.followMain) {
				usingCustomLayer[i] = true;
				width = custom.width;
				height = custom.height;
			} else {
				width = widthStep * i;
				width -= width % 2;
				height = heightStep * i;
				height -= height % 2;
			}

			if (videoSettings) {
				layerSettings = obs_data_create_from_json(obs_data_get_json(videoSettings));
				if (haveCustom && !custom.followMain) {
					obs_data_set_int(layerSettings, "bitrate", custom.bitrateKbps);
				} else {
					// Proportional to this layer's actual
					// pixel-area ratio against the main
					// output's resolution - see
					// WHIPSimulcastProportionalBitrate()
					// above.
					obs_data_set_int(layerSettings, "bitrate",
							  WHIPSimulcastProportionalBitrate(
								  videoBitrate, outputWidth, outputHeight, width,
								  height));
				}

				// The layer settings start as a copy of the
				// main encoder's, so without this every layer
				// would inherit the main layer's VBR ceiling
				// while carrying a fraction of its target - a
				// 480p layer with a few hundred Kbps of target
				// allowed to burst to the full 1080p peak.
				// Scaled by this layer's own target/main ratio
				// so the peak/target headroom the user
				// configured is the same on every layer,
				// whether the target came from the area
				// formula or from a custom per-layer value.
				//
				// Only meaningful in VBR; encoders that have no
				// max_bitrate setting (x264) read 0 here and
				// are left alone.
				const int64_t layerBitrate = obs_data_get_int(layerSettings, "bitrate");
				const int64_t mainMaxBitrate = obs_data_get_int(layerSettings, "max_bitrate");

				if (mainMaxBitrate > 0 && videoBitrate > 0) {
					int64_t layerMax = mainMaxBitrate * layerBitrate / videoBitrate;
					// A ceiling below the target is not a
					// ceiling.
					if (layerMax < layerBitrate)
						layerMax = layerBitrate;
					obs_data_set_int(layerSettings, "max_bitrate", layerMax);
				}
			}

			auto whip_simulcast_encoder =
				obs_video_encoder_create(encoderId, encoder_name.c_str(), layerSettings, nullptr);

			if (whip_simulcast_encoder) {
				obs_encoder_set_video(whip_simulcast_encoder, obs_get_video());
				obs_encoder_set_scaled_size(whip_simulcast_encoder, width, height);
				obs_encoder_set_gpu_scale_type(whip_simulcast_encoder, (obs_scale_type)rescaleFilter);
				whipSimulcastEncoders.push_back(whip_simulcast_encoder);
				obs_encoder_release(whip_simulcast_encoder);
			} else {
				blog(LOG_WARNING,
				     "Failed to create video streaming WHIP Simulcast encoders (BasicOutputHandler)");
			}
		}
	}

	// config is the same main->Config() passed to Create() - re-read
	// here (rather than cached from Create()) so a config change saved
	// while streaming (e.g. editing Settings > Stream mid-stream, which
	// the UI allows) is picked up on the next Update() the same way the
	// main encoder's own bitrate change already is.
	void Update(obs_data_t *videoSettings, int videoBitrate, config_t *config = nullptr)
	{
		uint32_t outputWidth = video_output_get_width(obs_get_video());
		uint32_t outputHeight = video_output_get_height(obs_get_video());

		for (size_t idx = 0; idx < whipSimulcastEncoders.size(); idx++) {
			OBSDataAutoRelease layerSettings = obs_data_create_from_json(obs_data_get_json(videoSettings));

			// idx counts up from 0 for the highest-quality
			// scaled-down layer (rid=1) - see Create(), which
			// appends in the same i=N-1..1 (i.e. slot=1..N-1)
			// order this loop walks forward through.
			WHIPSimulcastLayer custom;
			int slot = static_cast<int>(idx) + 1;
			if (GetWHIPSimulcastLayerConfig(config, slot, &custom) && !custom.followMain) {
				obs_data_set_int(layerSettings, "bitrate", custom.bitrateKbps);
			} else {
				// Same proportional-to-pixel-area formula as
				// Create()'s fallback, using this encoder's
				// *current* scaled size (obs_encoder_get_width/
				// height reflect whatever
				// obs_encoder_set_scaled_size last set it to in
				// Create() - Update() itself never changes it).
				uint32_t layerWidth = obs_encoder_get_width(whipSimulcastEncoders[idx]);
				uint32_t layerHeight = obs_encoder_get_height(whipSimulcastEncoders[idx]);
				obs_data_set_int(layerSettings, "bitrate",
						  WHIPSimulcastProportionalBitrate(videoBitrate, outputWidth,
										    outputHeight, layerWidth,
										    layerHeight));
			}

			obs_encoder_update(whipSimulcastEncoders[idx], layerSettings);
		}
	}

	void SetVideoFormat(enum video_format format)
	{
		for (auto enc : whipSimulcastEncoders)
			obs_encoder_set_preferred_video_format(enc, format);
	}

	// Create() binds each layer's encoder to obs_get_video() once, at
	// creation time. If video is reset (e.g. resolution change) afterward
	// without recreating the output handler, that video_t is destroyed and
	// the encoders are left pointing at a stale, freed video mix. The main
	// video/recording encoders get rebound on every SetupOutputs() call
	// (see AdvancedOutput::SetupOutputs/SimpleOutput::SetupOutputs); do the
	// same here so simulcast layers don't dereference a dangling mix
	// (obs_encoder_video_tex_active -> get_mix_for_video -> NULL) the next
	// time streaming starts.
	void SetVideo()
	{
		for (auto enc : whipSimulcastEncoders)
			obs_encoder_set_video(enc, obs_get_video());
	}

	void SetStreamOutput(obs_output_t *streamOutput)
	{
		for (size_t i = 0; i < whipSimulcastEncoders.size(); i++)
			obs_output_set_video_encoder2(streamOutput, whipSimulcastEncoders[i], i + 1);
	}

private:
	std::vector<OBSEncoder> whipSimulcastEncoders;
	std::vector<bool> usingCustomLayer;
};
