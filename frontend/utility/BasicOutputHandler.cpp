#include "BasicOutputHandler.hpp"
#include "AdvancedOutput.hpp"
#include "DegradeClient.hpp"
#include "SimpleOutput.hpp"

#include <abs-ts.h>

#include <algorithm>
#include <cmath>

#include <util/config-file.h>
#include <util/platform.h>

#include <utility/MultitrackVideoError.hpp>
#include <utility/StartMultiTrackVideoStreamingGuard.hpp>
#include <utility/VCamConfig.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QThreadPool>

using namespace std;

extern bool EncoderAvailable(const char *encoder);

volatile bool streaming_active = false;
volatile bool recording_active = false;
volatile bool recording_paused = false;
volatile bool replaybuf_active = false;
volatile bool virtualcam_active = false;

void OBSStreamStarting(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0)
		return;

	output->delayActive = true;
	QMetaObject::invokeMethod(output->main, "StreamDelayStarting", Q_ARG(int, sec));
}

void OBSStreamStopping(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0)
		QMetaObject::invokeMethod(output->main, "StreamStopping");
	else
		QMetaObject::invokeMethod(output->main, "StreamDelayStopping", Q_ARG(int, sec));
}

void OBSStartStreaming(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	output->streamingActive = true;
	os_atomic_set_bool(&streaming_active, true);
	QMetaObject::invokeMethod(output->main, "StreamingStart");
}

void OBSStopStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");

	QString arg_last_error = QString::fromUtf8(last_error);

	output->streamingActive = false;
	output->delayActive = false;
	output->multitrackVideoActive = false;
	output->StopCropStreaming();
	os_atomic_set_bool(&streaming_active, false);
	QMetaObject::invokeMethod(output->main, "StreamingStop", Q_ARG(int, code), Q_ARG(QString, arg_last_error));
}

void OBSStartRecording(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->recordingActive = true;
	os_atomic_set_bool(&recording_active, true);
	QMetaObject::invokeMethod(output->main, "RecordingStart");
}

void OBSStopRecording(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");

	QString arg_last_error = QString::fromUtf8(last_error);

	output->recordingActive = false;
	os_atomic_set_bool(&recording_active, false);
	os_atomic_set_bool(&recording_paused, false);
	QMetaObject::invokeMethod(output->main, "RecordingStop", Q_ARG(int, code), Q_ARG(QString, arg_last_error));
}

void OBSRecordStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "RecordStopping");
}

void OBSRecordFileChanged(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	const char *next_file = calldata_string(params, "next_file");

	QString arg_last_file = QString::fromUtf8(output->lastRecordingPath.c_str());

	QMetaObject::invokeMethod(output->main, "RecordingFileChanged", Q_ARG(QString, arg_last_file));

	output->lastRecordingPath = next_file;
}

void OBSStartReplayBuffer(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->replayBufferActive = true;
	os_atomic_set_bool(&replaybuf_active, true);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStart");
}

void OBSStopReplayBuffer(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->replayBufferActive = false;
	os_atomic_set_bool(&replaybuf_active, false);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStop", Q_ARG(int, code));
}

void OBSReplayBufferStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStopping");
}

void OBSReplayBufferSaved(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "ReplayBufferSaved", Qt::QueuedConnection);
}

static void OBSStartVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->virtualCamActive = true;
	os_atomic_set_bool(&virtualcam_active, true);
	QMetaObject::invokeMethod(output->main, "OnVirtualCamStart");
}

static void OBSStopVirtualCam(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->virtualCamActive = false;
	os_atomic_set_bool(&virtualcam_active, false);
	QMetaObject::invokeMethod(output->main, "OnVirtualCamStop", Q_ARG(int, code));
}

static void OBSDeactivateVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	output->DestroyVirtualCamView();
}

bool return_first_id(void *data, const char *id)
{
	const char **output = (const char **)data;

	*output = id;
	return false;
}

const char *GetStreamOutputType(const obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	const char *output = nullptr;

	if (!protocol) {
		blog(LOG_WARNING, "The service '%s' has no protocol set", obs_service_get_id(service));
		return nullptr;
	}

	if (!obs_is_output_protocol_registered(protocol)) {
		blog(LOG_WARNING, "The protocol '%s' is not registered", protocol);
		return nullptr;
	}

	/* Check if the service has a preferred output type */
	output = obs_service_get_preferred_output_type(service);
	if (output) {
		if ((obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0)
			return output;

		blog(LOG_WARNING, "The output '%s' is not registered, fallback to another one", output);
	}

	/* Otherwise, prefer first-party output types */
	if (can_use_output(protocol, "rtmp_output", "RTMP", "RTMPS")) {
		return "rtmp_output";
	} else if (can_use_output(protocol, "ffmpeg_hls_muxer", "HLS")) {
		return "ffmpeg_hls_muxer";
	} else if (can_use_output(protocol, "ffmpeg_mpegts_muxer", "SRT", "RIST")) {
		return "ffmpeg_mpegts_muxer";
	}

	/* If third-party protocol, use the first enumerated type */
	obs_enum_output_types_with_protocol(protocol, &output, return_first_id);
	if (output)
		return output;

	blog(LOG_WARNING, "No output compatible with the service '%s' is registered", obs_service_get_id(service));

	return nullptr;
}

BasicOutputHandler::BasicOutputHandler(OBSBasic *main_) : main(main_)
{
	if (main->vcamEnabled) {
		virtualCam = obs_output_create(VIRTUAL_CAM_ID, "virtualcam_output", nullptr, nullptr);

		signal_handler_t *signal = obs_output_get_signal_handler(virtualCam);
		startVirtualCam.Connect(signal, "start", OBSStartVirtualCam, this);
		stopVirtualCam.Connect(signal, "stop", OBSStopVirtualCam, this);
		deactivateVirtualCam.Connect(signal, "deactivate", OBSDeactivateVirtualCam, this);
	}

	auto service = main_->GetService();
	OBSDataAutoRelease settings = obs_service_get_settings(service);

	const char *service_protocol = obs_service_get_protocol(service);
	const bool is_whip = service_protocol && astrcmpi(service_protocol, "WHIP") == 0;
	const bool is_srt_custom = service_protocol && astrcmpi(service_protocol, "SRT") == 0 &&
					   strcmp(obs_service_get_id(service), "rtmp_custom") == 0;
	const bool is_ertmp = service_protocol && astrcmpi(service_protocol, "RTMP") == 0 &&
			      strcmp(obs_service_get_id(service), "rtmp_custom") == 0 &&
			      obs_data_get_bool(settings, "is_ertmp");

	// ERTMP uses the regular RTMP output (with the simulcast encoders
	// below); never route it through the multitrack-video output even if a
	// stale EnableMultitrackVideo value is still on disk.
	auto multitrack_enabled = !is_ertmp && config_get_bool(main->Config(), "Stream1", "EnableMultitrackVideo") &&
				  (obs_data_has_user_value(settings, "multitrack_video_configuration_url") ||
				   strcmp(obs_service_get_id(service), "rtmp_custom") == 0);

	if (multitrack_enabled)
		multitrackVideo = make_unique<MultitrackVideoOutput>();
	const int max_simulcast_layers = (is_srt_custom || is_ertmp) ? 4 : 5;
	const int configured_layers = config_get_int(main->Config(), "Stream1", "WHIPSimulcastTotalLayers");
	if ((is_whip || is_srt_custom || is_ertmp) && configured_layers > 1)
		whipSimulcastEncoders = make_unique<WHIPSimulcastEncoders>(max_simulcast_layers);
}

extern void log_vcam_changed(const VCamConfig &config, bool starting);

bool BasicOutputHandler::StartVirtualCam()
{
	if (!main->vcamEnabled)
		return false;

	bool typeIsProgram = main->vcamConfig.type == VCamOutputType::ProgramView;

	if (!virtualCamView && !typeIsProgram)
		virtualCamView = obs_view_create();

	UpdateVirtualCamOutputSource();

	if (!virtualCamVideo) {
		virtualCamVideo = typeIsProgram ? obs_get_video() : obs_view_add(virtualCamView);

		if (!virtualCamVideo)
			return false;
	}

	obs_output_set_media(virtualCam, virtualCamVideo, obs_get_audio());
	if (!Active())
		SetupOutputs();

	bool success = obs_output_start(virtualCam);
	if (!success) {
		QString errorReason;

		const char *error = obs_output_get_last_error(virtualCam);
		if (error) {
			errorReason = QT_UTF8(error);
		} else {
			errorReason = QTStr("Output.StartFailedGeneric");
		}

		QMessageBox::critical(main, QTStr("Output.StartVirtualCamFailed"), errorReason);

		DestroyVirtualCamView();
	}

	log_vcam_changed(main->vcamConfig, true);

	return success;
}

void BasicOutputHandler::StopVirtualCam()
{
	if (main->vcamEnabled) {
		obs_output_stop(virtualCam);
	}
}

bool BasicOutputHandler::VirtualCamActive() const
{
	if (main->vcamEnabled) {
		return obs_output_active(virtualCam);
	}
	return false;
}

void BasicOutputHandler::UpdateVirtualCamOutputSource()
{
	if (!main->vcamEnabled || !virtualCamView)
		return;

	OBSSourceAutoRelease source;

	switch (main->vcamConfig.type) {
	case VCamOutputType::Invalid:
	case VCamOutputType::ProgramView:
		DestroyVirtualCameraScene();
		return;
	case VCamOutputType::PreviewOutput: {
		DestroyVirtualCameraScene();
		OBSSource s = main->GetCurrentSceneSource();
		obs_source_get_ref(s);
		source = s.Get();
		break;
	}
	case VCamOutputType::SceneOutput:
		DestroyVirtualCameraScene();
		source = obs_get_source_by_name(main->vcamConfig.scene.c_str());
		break;
	case VCamOutputType::SourceOutput:
		OBSSourceAutoRelease s = obs_get_source_by_name(main->vcamConfig.source.c_str());

		if (!vCamSourceScene)
			vCamSourceScene = obs_scene_create_private("vcam_source");
		source = obs_source_get_ref(obs_scene_get_source(vCamSourceScene));

		if (vCamSourceSceneItem && (obs_sceneitem_get_source(vCamSourceSceneItem) != s)) {
			obs_sceneitem_remove(vCamSourceSceneItem);
			vCamSourceSceneItem = nullptr;
		}

		if (!vCamSourceSceneItem) {
			vCamSourceSceneItem = obs_scene_add(vCamSourceScene, s);

			obs_sceneitem_set_bounds_type(vCamSourceSceneItem, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(vCamSourceSceneItem, OBS_ALIGN_CENTER);

			const struct vec2 size = {
				(float)obs_source_get_width(source),
				(float)obs_source_get_height(source),
			};
			obs_sceneitem_set_bounds(vCamSourceSceneItem, &size);
		}
		break;
	}

	OBSSourceAutoRelease current = obs_view_get_source(virtualCamView, 0);
	if (source != current)
		obs_view_set_source(virtualCamView, 0, source);
}

void BasicOutputHandler::DestroyVirtualCamView()
{
	if (main->vcamConfig.type == VCamOutputType::ProgramView) {
		virtualCamVideo = nullptr;
		return;
	}

	obs_view_remove(virtualCamView);
	obs_view_set_source(virtualCamView, 0, nullptr);
	virtualCamVideo = nullptr;

	obs_view_destroy(virtualCamView);
	virtualCamView = nullptr;

	DestroyVirtualCameraScene();
}

void BasicOutputHandler::DestroyVirtualCameraScene()
{
	if (!vCamSourceScene)
		return;

	obs_scene_release(vCamSourceScene);
	vCamSourceScene = nullptr;
	vCamSourceSceneItem = nullptr;
}

const char *FindAudioEncoderFromCodec(const char *type)
{
	const char *alt_enc_id = nullptr;
	size_t i = 0;

	while (obs_enum_encoder_types(i++, &alt_enc_id)) {
		const char *codec = obs_get_encoder_codec(alt_enc_id);
		if (strcmp(type, codec) == 0) {
			return alt_enc_id;
		}
	}

	return nullptr;
}

void clear_archive_encoder(obs_output_t *output, const char *expected_name)
{
	obs_encoder_t *last = obs_output_get_audio_encoder(output, 1);
	bool clear = false;

	/* ensures that we don't remove twitch's soundtrack encoder */
	if (last) {
		const char *name = obs_encoder_get_name(last);
		clear = name && strcmp(name, expected_name) == 0;
		obs_encoder_release(last);
	}

	if (clear)
		obs_output_set_audio_encoder(output, nullptr, 1);
}

void BasicOutputHandler::SetupAutoRemux(const char *&container)
{
	bool autoRemux = config_get_bool(main->Config(), "Video", "AutoRemux");
	if (autoRemux && strcmp(container, "mp4") == 0)
		container = "mkv";
}

std::string BasicOutputHandler::GetRecordingFilename(const char *path, const char *container, bool noSpace,
						     bool overwrite, const char *format, bool ffmpeg)
{
	if (!ffmpeg)
		SetupAutoRemux(container);

	string dst = GetOutputFilename(path, container, noSpace, overwrite, format);
	lastRecordingPath = dst;
	return dst;
}

extern std::string DeserializeConfigText(const char *text);

std::shared_future<void> BasicOutputHandler::SetupMultitrackVideo(obs_service_t *service, std::string audio_encoder_id,
								  size_t main_audio_mixer,
								  std::optional<size_t> vod_track_mixer,
								  std::function<void(std::optional<bool>)> continuation)
{
	auto start_streaming_guard = std::make_shared<StartMultitrackVideoStreamingGuard>();
	if (!multitrackVideo) {
		continuation(std::nullopt);
		return start_streaming_guard->GetFuture();
	}

	multitrackVideoActive = false;

	streamDelayStarting.Disconnect();
	streamStopping.Disconnect();
	startStreaming.Disconnect();
	stopStreaming.Disconnect();

	bool is_custom = strncmp("rtmp_custom", obs_service_get_type(service), 11) == 0;

	std::optional<std::string> custom_config = std::nullopt;
	if (config_get_bool(main->Config(), "Stream1", "MultitrackVideoConfigOverrideEnabled"))
		custom_config = DeserializeConfigText(
			config_get_string(main->Config(), "Stream1", "MultitrackVideoConfigOverride"));

	std::optional<QString> extraCanvasUUID;
	const char *uuid = config_get_string(main->Config(), "Stream1", "MultitrackExtraCanvas");
	if (uuid && *uuid) {
		extraCanvasUUID = uuid;
	}

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	QString key = obs_data_get_string(settings, "key");

	const char *service_name = "<unknown>";
	if (is_custom && obs_data_has_user_value(settings, "service_name")) {
		service_name = obs_data_get_string(settings, "service_name");
	} else if (!is_custom) {
		service_name = obs_data_get_string(settings, "service");
	}

	std::optional<std::string> custom_rtmp_url;
	std::optional<bool> use_rtmps;
	auto server = obs_data_get_string(settings, "server");
	if (strncmp(server, "auto", 4) != 0) {
		custom_rtmp_url = server;
	} else {
		QString server_ = server;
		use_rtmps = server_.contains("rtmps", Qt::CaseInsensitive);
	}

	auto service_custom_server = obs_data_get_bool(settings, "using_custom_server");
	if (custom_rtmp_url.has_value()) {
		blog(LOG_INFO, "Using %sserver '%s'", service_custom_server ? "custom " : "", custom_rtmp_url->c_str());
	}

	auto maximum_aggregate_bitrate =
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto")
			? std::nullopt
			: std::make_optional<uint32_t>(
				  config_get_int(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrate"));

	auto maximum_video_tracks = config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracksAuto")
					    ? std::nullopt
					    : std::make_optional<uint32_t>(config_get_int(
						      main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracks"));

	auto stream_dump_config = GenerateMultitrackVideoStreamDumpConfig();

	auto continue_on_main_thread = [&, start_streaming_guard, service = OBSService{service},
					continuation =
						std::move(continuation)](std::optional<MultitrackVideoError> error) {
		if (error) {
			OBSDataAutoRelease service_settings = obs_service_get_settings(service);
			auto multitrack_video_name = QTStr("Basic.Settings.Stream.MultitrackVideoLabel");
			if (obs_data_has_user_value(service_settings, "multitrack_video_name")) {
				multitrack_video_name = obs_data_get_string(service_settings, "multitrack_video_name");
			}

			multitrackVideoActive = false;
			if (!error->ShowDialog(main, multitrack_video_name))
				return continuation(false);
			return continuation(std::nullopt);
		}

		multitrackVideoActive = true;

		auto signal_handler = multitrackVideo->StreamingSignalHandler();

		streamDelayStarting.Connect(signal_handler, "starting", OBSStreamStarting, this);
		streamStopping.Connect(signal_handler, "stopping", OBSStreamStopping, this);

		startStreaming.Connect(signal_handler, "start", OBSStartStreaming, this);
		stopStreaming.Connect(signal_handler, "stop", OBSStopStreaming, this);
		return continuation(true);
	};

	QThreadPool::globalInstance()->start([=, main = main, multitrackVideo = multitrackVideo.get(),
					      service_name = std::string{service_name}, service = OBSService{service},
					      stream_dump_config = OBSData{stream_dump_config},
					      start_streaming_guard = start_streaming_guard]() mutable {
		std::optional<MultitrackVideoError> error;
		try {
			multitrackVideo->PrepareStreaming(main, service_name.c_str(), service, custom_rtmp_url, key,
							  audio_encoder_id.c_str(), maximum_aggregate_bitrate,
							  maximum_video_tracks, custom_config, stream_dump_config,
							  main_audio_mixer, vod_track_mixer, use_rtmps,
							  extraCanvasUUID);
		} catch (const MultitrackVideoError &error_) {
			error.emplace(error_);
		}

		QMetaObject::invokeMethod(main, [=] { continue_on_main_thread(error); });
	});

	return start_streaming_guard->GetFuture();
}

OBSDataAutoRelease BasicOutputHandler::GenerateMultitrackVideoStreamDumpConfig()
{
	auto stream_dump_enabled = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpEnabled");

	if (!stream_dump_enabled)
		return nullptr;

	const char *path = config_get_string(main->Config(), "SimpleOutput", "FilePath");
	bool noSpace = config_get_bool(main->Config(), "SimpleOutput", "FileNameWithoutSpace");
	const char *filenameFormat = config_get_string(main->Config(), "Output", "FilenameFormatting");
	bool overwriteIfExists = config_get_bool(main->Config(), "Output", "OverwriteIfExists");
	bool useMP4 = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpAsMP4");

	string f;

	OBSDataAutoRelease settings = obs_data_create();
	f = GetFormatString(filenameFormat, nullptr, nullptr);
	string strPath = GetRecordingFilename(path, useMP4 ? "mp4" : "flv", noSpace, overwriteIfExists, f.c_str(),
					      // never remux stream dump
					      false);
	obs_data_set_string(settings, "path", strPath.c_str());

	if (useMP4) {
		obs_data_set_bool(settings, "use_mp4", true);
		obs_data_set_string(settings, "muxer_settings", "write_encoder_info=1");
	}

	return settings;
}

BasicOutputHandler::~BasicOutputHandler()
{
	StopDegrade();
	StopCropStreaming(true);
	CleanupCropResources();
}

static void StopOutputAndWait(obs_output_t *output)
{
	if (!output)
		return;

	if (obs_output_active(output)) {
		obs_output_force_stop(output);

		// obs_output_force_stop() is not guaranteed to have fully torn
		// down the encoder/view relation yet. Wait briefly (bounded) so
		// the crop view can be removed without racing an active encoder.
		for (int i = 0; i < 500 && obs_output_active(output); i++)
			os_sleep_ms(10);
	}
}

static void ScaleCropBitrate(obs_data_t *settings, const char *key, double ratio)
{
	const int value = (int)obs_data_get_int(settings, key);
	if (value <= 0)
		return;

	int scaled = (int)std::lround((double)value * ratio);
	if (scaled < 1)
		scaled = 1;

	obs_data_set_int(settings, key, scaled);
}

void BasicOutputHandler::CleanupCropResources()
{
	if (cropStreamOutput)
		StopOutputAndWait(cropStreamOutput);

	cropStreamingActive = false;

	cropVideoEncoder = nullptr;
	cropAudioEncoder = nullptr;

	if (cropView) {
		obs_view_set_source(cropView, 0, nullptr);
		obs_view_remove(cropView);
		obs_view_destroy(cropView);
		cropView = nullptr;
	}
	cropVideo = nullptr;

	if (cropScene) {
		obs_scene_release(cropScene);
		cropScene = nullptr;
		cropSceneItem = nullptr;
	}

	cropStreamOutput = nullptr;
	cropService = nullptr;
}

void BasicOutputHandler::StopCropStreaming(bool force)
{
	if (!cropStreamOutput)
		return;

	if (force)
		obs_output_force_stop(cropStreamOutput);
	else
		obs_output_stop(cropStreamOutput);

	cropStreamingActive = false;
}

void BasicOutputHandler::StartCropStreaming(obs_service_t *service)
{
	if (!service)
		return;

	if (cropStreamOutput && obs_output_active(cropStreamOutput))
		return;

	// Drop resources from a previous run. Safe here because this is only
	// reached after the main stream has been (re)started, i.e. any old
	// crop output has already been stopped.
	CleanupCropResources();

	if (!config_get_bool(main->Config(), "Video", "CropStreamEnabled"))
		return;

	if (multitrackVideo && multitrackVideoActive) {
		blog(LOG_WARNING, "[crop] crop stream is not supported with multitrack video output");
		return;
	}

	auto mainOutput = StreamingOutput();
	if (!mainOutput)
		return;

	obs_encoder_t *mainVideo = obs_output_get_video_encoder(mainOutput);
	obs_encoder_t *mainAudio = obs_output_get_audio_encoder(mainOutput, 0);
	if (!mainVideo) {
		blog(LOG_WARNING, "[crop] main stream has no video encoder, skipping crop stream");
		return;
	}

	const uint32_t mainW = obs_encoder_get_width(mainVideo);
	const uint32_t mainH = obs_encoder_get_height(mainVideo);
	if (!mainW || !mainH) {
		blog(LOG_WARNING, "[crop] main stream video size is unavailable, skipping crop stream");
		return;
	}

	int cropX = (int)config_get_int(main->Config(), "Video", "CropStreamX");
	int cropY = (int)config_get_int(main->Config(), "Video", "CropStreamY");
	int cropW = (int)config_get_int(main->Config(), "Video", "CropStreamWidth");
	int cropH = (int)config_get_int(main->Config(), "Video", "CropStreamHeight");
	int cropFps = (int)config_get_int(main->Config(), "Video", "CropStreamFPS");
	if (cropFps <= 0)
		cropFps = 15;
	cropFps = std::clamp(cropFps, 1, 120);

	if (cropW <= 0 || cropH <= 0) {
		blog(LOG_WARNING, "[crop] invalid crop size %dx%d, skipping crop stream", cropW, cropH);
		return;
	}

	cropX = std::clamp(cropX, 0, (int)mainW - 1);
	cropY = std::clamp(cropY, 0, (int)mainH - 1);
	cropW = std::min(cropW, (int)mainW - cropX);
	cropH = std::min(cropH, (int)mainH - cropY);

	// Keep the encoded dimensions even; most hardware encoders reject odd
	// dimensions for 4:2:0 formats.
	cropW &= ~1;
	cropH &= ~1;
	if (cropW < 2 || cropH < 2) {
		blog(LOG_WARNING, "[crop] crop size %dx%d is too small, skipping crop stream", cropW, cropH);
		return;
	}

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height) {
		blog(LOG_WARNING, "[crop] video info is unavailable, skipping crop stream");
		return;
	}

	// The frame rate divisor only supports integer divisions of the main
	// stream's frame rate (the graphics thread renders all views at the
	// main rate), so use the closest achievable rate for the encoder while
	// scaling the bitrate by the configured crop/main frame-rate ratio
	// (capped at 1, since the crop stream cannot run faster than main).
	const double mainFps = (double)ovi.fps_num / (double)ovi.fps_den;
	uint32_t fpsDivisor = 1;
	if (mainFps > 0.0 && cropFps < mainFps)
		fpsDivisor = (uint32_t)std::max(1LL, (long long)std::lround(mainFps / (double)cropFps));
	const double actualCropFps = mainFps > 0.0 ? mainFps / (double)fpsDivisor : (double)cropFps;
	const double fpsRatio =
		mainFps > 0.0 ? std::min((double)cropFps, mainFps) / mainFps : 1.0;

	// Config coordinates are in main-output pixels (same convention as the
	// manual ROI); the view renders the program source in canvas pixels.
	const double scaleX = (double)ovi.base_width / (double)mainW;
	const double scaleY = (double)ovi.base_height / (double)mainH;

	int baseX = (int)std::lround((double)cropX * scaleX);
	int baseY = (int)std::lround((double)cropY * scaleY);
	int baseW = (int)std::lround((double)cropW * scaleX);
	int baseH = (int)std::lround((double)cropH * scaleY);

	baseX = std::clamp(baseX, 0, (int)ovi.base_width - 1);
	baseY = std::clamp(baseY, 0, (int)ovi.base_height - 1);
	baseW = std::clamp(baseW, 1, (int)ovi.base_width - baseX);
	baseH = std::clamp(baseH, 1, (int)ovi.base_height - baseY);

	obs_video_info cropOvi = ovi;
	cropOvi.base_width = (uint32_t)baseW;
	cropOvi.base_height = (uint32_t)baseH;
	cropOvi.output_width = (uint32_t)cropW;
	cropOvi.output_height = (uint32_t)cropH;

	cropView = obs_view_create();
	if (!cropView) {
		blog(LOG_WARNING, "[crop] failed to create view");
		CleanupCropResources();
		return;
	}

	cropVideo = obs_view_add2(cropView, &cropOvi);
	if (!cropVideo) {
		blog(LOG_WARNING, "[crop] failed to create video mix");
		CleanupCropResources();
		return;
	}

	cropScene = obs_scene_create_private("crop_stream_scene");
	if (!cropScene) {
		blog(LOG_WARNING, "[crop] failed to create scene");
		CleanupCropResources();
		return;
	}

	OBSSourceAutoRelease programSource = obs_get_output_source(0);
	if (!programSource) {
		blog(LOG_WARNING, "[crop] no program output source");
		CleanupCropResources();
		return;
	}

	cropSceneItem = obs_scene_add(cropScene, programSource);
	if (!cropSceneItem) {
		blog(LOG_WARNING, "[crop] failed to add program source to crop scene");
		CleanupCropResources();
		return;
	}

	vec2 offset;
	vec2_set(&offset, -(float)baseX, -(float)baseY);
	obs_sceneitem_set_pos(cropSceneItem, &offset);

	obs_view_set_source(cropView, 0, obs_scene_get_source(cropScene));

	// Fixed stream-key pattern: main key + "-h5".
	OBSDataAutoRelease serviceSettings = obs_service_get_settings(service);
	OBSDataAutoRelease cropSettings = obs_data_create();
	obs_data_apply(cropSettings, serviceSettings);

	auto appendKey = [&](const char *field) -> bool {
		const char *value = obs_data_get_string(cropSettings, field);
		if (!value || !*value)
			return false;

		std::string cropKey = value;
		cropKey += "-h5";
		obs_data_set_string(cropSettings, field, cropKey.c_str());
		blog(LOG_INFO, "[crop] stream key: %s", cropKey.c_str());
		return true;
	};

	if (!appendKey("key") && !appendKey("stream_id")) {
		blog(LOG_WARNING, "[crop] main service has no stream key, skipping crop stream");
		CleanupCropResources();
		return;
	}

	cropService = obs_service_create(obs_service_get_type(service), "crop_service", cropSettings, nullptr);
	if (!cropService) {
		blog(LOG_WARNING, "[crop] failed to create crop service");
		CleanupCropResources();
		return;
	}

	const char *outputType = GetStreamOutputType(cropService);
	if (!outputType) {
		blog(LOG_WARNING, "[crop] no output type for crop service");
		CleanupCropResources();
		return;
	}

	OBSDataAutoRelease mainVideoSettings = obs_encoder_get_settings(mainVideo);
	OBSDataAutoRelease cropVideoSettings = obs_data_create();
	obs_data_apply(cropVideoSettings, mainVideoSettings);

	const double areaRatio =
		((double)cropW * (double)cropH) / ((double)mainW * (double)mainH);
	const double bitrateRatio = areaRatio * fpsRatio;
	ScaleCropBitrate(cropVideoSettings, "bitrate", bitrateRatio);
	ScaleCropBitrate(cropVideoSettings, "max_bitrate", bitrateRatio);
	ScaleCropBitrate(cropVideoSettings, "maxrate", bitrateRatio);
	ScaleCropBitrate(cropVideoSettings, "bufsize", bitrateRatio);
	ScaleCropBitrate(cropVideoSettings, "vbv-maxrate", bitrateRatio);
	ScaleCropBitrate(cropVideoSettings, "vbv-bufsize", bitrateRatio);

	cropVideoEncoder = obs_video_encoder_create(obs_encoder_get_id(mainVideo), "crop_stream_video",
						   cropVideoSettings, nullptr);
	if (!cropVideoEncoder) {
		blog(LOG_WARNING, "[crop] failed to create video encoder");
		CleanupCropResources();
		return;
	}
	obs_encoder_release(cropVideoEncoder);

	obs_encoder_set_preferred_video_format(cropVideoEncoder,
					       obs_encoder_get_preferred_video_format(mainVideo));
	obs_encoder_set_video(cropVideoEncoder, cropVideo);
	if (fpsDivisor > 1)
		obs_encoder_set_frame_rate_divisor(cropVideoEncoder, fpsDivisor);

	if (mainAudio) {
		OBSDataAutoRelease mainAudioSettings = obs_encoder_get_settings(mainAudio);
		cropAudioEncoder = obs_audio_encoder_create(obs_encoder_get_id(mainAudio), "crop_stream_audio",
							    mainAudioSettings,
							    obs_encoder_get_mixer_index(mainAudio), nullptr);
		if (cropAudioEncoder) {
			obs_encoder_release(cropAudioEncoder);
			obs_encoder_set_audio(cropAudioEncoder, obs_get_audio());
		} else {
			blog(LOG_WARNING, "[crop] failed to create audio encoder, continuing without audio");
		}
	}

	cropStreamOutput = obs_output_create(outputType, "crop_stream", nullptr, nullptr);
	if (!cropStreamOutput) {
		blog(LOG_WARNING, "[crop] failed to create output '%s'", outputType);
		CleanupCropResources();
		return;
	}

	// Main stream outputs inject the OBS absolute-timestamp SEI (see
	// abs-ts.h); do the same for the crop stream so mmx/players can compute
	// p2p delay for the -h5 path too.
	obs_output_add_packet_callback(cropStreamOutput, abs_ts_sei_inject, nullptr);

	obs_output_set_video_encoder(cropStreamOutput, cropVideoEncoder);
	if (cropAudioEncoder)
		obs_output_set_audio_encoder(cropStreamOutput, cropAudioEncoder, 0);
	obs_output_set_service(cropStreamOutput, cropService);

	OBSDataAutoRelease outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "bind_ip", config_get_string(main->Config(), "Output", "BindIP"));
	obs_data_set_string(outputSettings, "ip_family",
			    config_get_string(main->Config(), "Output", "IPFamily"));
#ifdef _WIN32
	obs_data_set_bool(outputSettings, "new_socket_loop_enabled",
			  config_get_bool(main->Config(), "Output", "NewSocketLoopEnable"));
	obs_data_set_bool(outputSettings, "low_latency_mode_enabled",
			  config_get_bool(main->Config(), "Output", "LowLatencyEnable"));
#endif
	obs_data_set_bool(outputSettings, "dyn_bitrate",
			  config_get_bool(main->Config(), "Output", "DynamicBitrate"));
	obs_output_update(cropStreamOutput, outputSettings);

	const bool reconnect = config_get_bool(main->Config(), "Output", "Reconnect");
	const int retryDelay = config_get_uint(main->Config(), "Output", "RetryDelay");
	const int maxRetries = reconnect ? (int)config_get_uint(main->Config(), "Output", "MaxRetries") : 0;
	obs_output_set_reconnect_settings(cropStreamOutput, maxRetries, retryDelay);
	obs_output_set_delay(cropStreamOutput, 0, 0);

	obs_queue_task(
		OBS_TASK_GRAPHICS, [](void *) {}, nullptr, true);

	ForceZeroBFrames(cropStreamOutput);

	blog(LOG_INFO,
	     "[crop] starting %s output at %dx%d (x=%d y=%d, fps %d requested / %.2f actual, divisor %u, "
	     "bitrate ratio %.4f)",
	     outputType, cropW, cropH, cropX, cropY, cropFps, actualCropFps, fpsDivisor, bitrateRatio);

	if (!obs_output_start(cropStreamOutput)) {
		const char *error = obs_output_get_last_error(cropStreamOutput);
		blog(LOG_WARNING, "[crop] output failed to start%s%s", error && *error ? ": " : "",
		     error && *error ? error : "");
		StopOutputAndWait(cropStreamOutput);
		CleanupCropResources();
		return;
	}

	cropStreamingActive = true;
}

void BasicOutputHandler::StartDegrade(obs_service_t *service, obs_output_t *output)
{
	StopDegrade();

	if (!service || !output)
		return;

	// Multitrack video manages its own outputs/encoders; leave it alone.
	if (multitrackVideo && multitrackVideoActive)
		return;

	const char *protocol = obs_service_get_protocol(service);
	const bool is_whip = protocol && astrcmpi(protocol, "WHIP") == 0;
	const bool is_srt = protocol && astrcmpi(protocol, "SRT") == 0;
	if (!is_whip && !is_srt)
		return;

	std::string ws_url = DegradeClient::WebSocketUrlForService(service);
	if (ws_url.empty()) {
		blog(LOG_WARNING, "[degrade] cannot derive degrade WS URL from the %s endpoint", protocol);
		return;
	}

	std::lock_guard<std::mutex> lk(degradeMutex);

	degradeEncoders.clear();
	for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(output, i);
		if (!enc)
			break;
		degradeEncoders.emplace_back(enc);
	}

	if (degradeEncoders.empty())
		return;

	degradeFullLayers = (int)degradeEncoders.size();
	degradeCurLayers = degradeFullLayers;
	degradeTargetLayers = degradeFullLayers;
	degradeCurPct = 100;
	degradeActive = true;
	degradeRestartQueued.store(false);

	// Loss-driven restart policy, from Settings > Advanced ("RTP丢包重启");
	// defaults are registered in OBSBasic::OBSInit, so config_get_* returns
	// them when unset. mmx pushes receiver-measured loss (LOSS_STATS) and
	// the executor restarts the output once it stays above the threshold
	// for the configured duration.
	{
		config_t *cfg = App()->GetAppConfig();
		degradeLossRestartEnabled = config_get_bool(cfg, "General", "RtpLossRestart");
		degradeLossRestartThresholdPct =
			config_get_double(cfg, "General", "RtpLossRestartThresholdPct");
		degradeLossRestartDurationMs =
			(uint64_t)config_get_int(cfg, "General", "RtpLossRestartDurationSec") * 1000ULL;
	}
	degradeLossHighSinceNs = 0;

	blog(LOG_INFO, "[degrade] executor starting for %s (%d layer(s), ws=%s, loss_restart=%s %.2f%%/%llums)",
	     protocol, degradeFullLayers, ws_url.c_str(), degradeLossRestartEnabled ? "on" : "off",
	     degradeLossRestartThresholdPct, (unsigned long long)degradeLossRestartDurationMs);

	degradeClient = std::make_unique<DegradeClient>();
	degradeClient->Start(ws_url, [this](int layers, int pct) { ApplyDegradeTarget(layers, pct); },
			     [this](const DegradeClient::LossStats &s) {
				     ApplyDegradeLoss(s.proto.c_str(), s.unrecovered_pct, s.raw_pct,
						      s.bitrate_mbps, s.rtt_ms);
			     },
			     [this](const std::string &reason) { ApplyDegradeReconnect(reason.c_str()); });
}

void BasicOutputHandler::StopDegrade()
{
	std::unique_ptr<DegradeClient> client;
	{
		std::lock_guard<std::mutex> lk(degradeMutex);
		degradeActive = false;
		degradeRestartQueued.store(false);
		client = std::move(degradeClient);
		degradeEncoders.clear();
		degradeFullLayers = 0;
		degradeCurLayers = 0;
		degradeTargetLayers = 0;
		degradeCurPct = 100;
	}

	// Join outside the lock: a callback still in flight needs degradeMutex.
	if (client)
		client->Stop();
}

// WebRTC forwarding (origin -> record/edge) cannot carry H.264 with B-frames,
// so force them off on every video encoder of the streaming output before it
// starts. Different encoders name the option differently (NVENC/x264: "bf",
// QSV: "bframes"), so set all known keys.
void BasicOutputHandler::ForceZeroBFrames(obs_output_t *output)
{
	if (!output)
		return;

	for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(output, i);
		if (!enc)
			break;

		OBSDataAutoRelease settings = obs_encoder_get_settings(enc);
		const int bf = (int)obs_data_get_int(settings, "bf");
		const int bframes = (int)obs_data_get_int(settings, "bframes");
		const int maxbf = (int)obs_data_get_int(settings, "max_b_frames");

		// Set unconditionally: when a key is absent the encoder's own default
		// (NVENC 2, QSV 3) still applies, so only checking the stored value
		// would miss it.
		obs_data_set_int(settings, "bf", 0);
		obs_data_set_int(settings, "bframes", 0);
		obs_data_set_int(settings, "max_b_frames", 0);
		obs_encoder_update(enc, settings);

		if (bf != 0 || bframes != 0 || maxbf != 0)
			blog(LOG_WARNING,
			     "[bf] forced B-frames=0 on streaming encoder '%s' (was bf=%d bframes=%d max_b_frames=%d)",
			     obs_encoder_get_id(enc), bf, bframes, maxbf);
	}
}

void BasicOutputHandler::ApplyDegradeTarget(int layers, int bitratePercent)
{
	std::lock_guard<std::mutex> lk(degradeMutex);

	if (!degradeActive)
		return;

	const int full = degradeFullLayers;
	int want = layers > 0 ? std::min(layers, full) : full;
	want = std::clamp(want, 1, full > 0 ? full : 1);
	bitratePercent = std::clamp(bitratePercent, 1, 100);

	const bool layers_changed = (full > 1) && (want != degradeTargetLayers);
	const bool pct_changed = (bitratePercent != degradeCurPct);

	blog(LOG_INFO, "[degrade] TARGET layers=%d bitrate=%d%% (current layers=%d bitrate=%d%%)", layers,
	     bitratePercent, degradeCurLayers, degradeCurPct);

	// Idempotency (§R3): already at target -> do nothing, no restart.
	if (!layers_changed && !pct_changed)
		return;

	OBSOutputAutoRelease output = StreamingOutput();
	if (!output) {
		blog(LOG_WARNING, "[degrade] no streaming output to apply target");
		return;
	}

	if (pct_changed) {
		ApplyDegradeBitrate(bitratePercent);
		degradeCurPct = bitratePercent;
	}

	if (layers_changed) {
		// §R4: merge back-to-back layer changes into a single restart,
		// and run it on the Qt main thread - see PerformDegradeRestart.
		blog(LOG_INFO, "[degrade] layer change %d -> %d requested, queuing restart", degradeCurLayers, want);
		degradeTargetLayers = want;
		QueueDegradeRestart();
	}
}

// Queues one restart of the streaming output on the Qt main thread,
// collapsing concurrent requests (TARGET_STATE layer change and/or a
// loss-driven reconnect) into a single stop+start. Caller must hold
// degradeMutex.
void BasicOutputHandler::QueueDegradeRestart()
{
	if (degradeRestartQueued.exchange(true))
		return;

	OBSBasic *m = main;
	QMetaObject::invokeMethod(
		m,
		[this, m]() {
			// The handler may have been torn down (profile switch /
			// app exit) before this ran.
			if (!m || m->outputHandler.get() != this)
				return;
			PerformDegradeRestart();
		},
		Qt::QueuedConnection);
}

// Loss-driven restart (LOSS_STATS): mmx reports its receiver-measured
// unrecovered drop rate every sample. When it stays above the configured
// threshold for the configured duration, restart the output so the
// publisher reconnects - the same action the old mmx-side fast reconnect
// took (close the connection), now decided here from mmx's numbers.
void BasicOutputHandler::ApplyDegradeLoss(const char *proto, double unrecoveredPct, double rawPct,
					  double bitrateMbps, double rttMs)
{
	std::lock_guard<std::mutex> lk(degradeMutex);

	if (!degradeActive || !degradeLossRestartEnabled)
		return;

	const uint64_t now = os_gettime_ns();

	// At/below the threshold (including the deadband): the run is broken.
	if (unrecoveredPct <= degradeLossRestartThresholdPct) {
		degradeLossHighSinceNs = 0;
		return;
	}

	// Above the threshold: start the run, then restart once it has lasted
	// at least degradeLossRestartDurationMs.
	if (degradeLossHighSinceNs == 0) {
		degradeLossHighSinceNs = now;
		blog(LOG_INFO, "[degrade] LOSS_STATS %s unrecovered=%.2f%% > %.2f%% - restart timer started",
		     proto ? proto : "?", unrecoveredPct, degradeLossRestartThresholdPct);
		return;
	}

	if (now - degradeLossHighSinceNs < degradeLossRestartDurationMs * 1000000ULL)
		return;

	blog(LOG_WARNING,
	     "[degrade] LOSS_STATS %s unrecovered=%.2f%% (raw=%.2f%%, %.2f Mbps, rtt %.1fms) above %.2f%% for %llums - restarting",
	     proto ? proto : "?", unrecoveredPct, rawPct, bitrateMbps, rttMs, degradeLossRestartThresholdPct,
	     (unsigned long long)((now - degradeLossHighSinceNs) / 1000000ULL));

	// Re-arm so the next restart needs another full duration.
	degradeLossHighSinceNs = now;

	// Same restart machinery as a layer change, at the current layer count.
	degradeTargetLayers = degradeCurLayers;
	QueueDegradeRestart();
}

// RECONNECT command from mmx: the tuned SRT latency now exceeds this
// connection's negotiated window, so restart the output now to re-handshake
// and apply it. This replaces mmx silently closing the connection (which the
// sender only notices after its idle timeout) with an immediate reconnect.
void BasicOutputHandler::ApplyDegradeReconnect(const char *reason)
{
	std::lock_guard<std::mutex> lk(degradeMutex);

	if (!degradeActive)
		return;

	blog(LOG_WARNING, "[degrade] RECONNECT requested%s%s - restarting stream",
	     (reason && reason[0]) ? ": " : "", (reason && reason[0]) ? reason : "");

	degradeTargetLayers = degradeCurLayers;
	QueueDegradeRestart();
}

void BasicOutputHandler::PerformDegradeRestart()
{
	std::lock_guard<std::mutex> lk(degradeMutex);

	degradeRestartQueued.store(false);
	if (!degradeActive)
		return;

	OBSOutputAutoRelease output = StreamingOutput();
	if (!output)
		return;

	const int want = degradeTargetLayers;
	blog(LOG_INFO, "[degrade] restarting stream at %d layer(s)", want);

	obs_output_stop(output);
	for (int i = 0; i < 300 && obs_output_active(output); i++)
		os_sleep_ms(10);

	if (obs_output_active(output)) {
		blog(LOG_WARNING, "[degrade] output did not stop in time, keeping current layer count");
		degradeTargetLayers = degradeCurLayers;
		return;
	}

	RebindDegradeLayers(output, want);
	degradeCurLayers = want;

	ForceZeroBFrames(output);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		blog(LOG_WARNING, "[degrade] failed to restart output%s%s", err ? ": " : "", err ? err : "");
	}
}

void BasicOutputHandler::ApplyDegradeBitrate(int bitratePercent)
{
	for (auto &encoder : degradeEncoders) {
		obs_encoder_t *enc = encoder.Get();
		if (!enc)
			continue;

		OBSDataAutoRelease settings = obs_encoder_get_settings(enc);
		int bitrate = (int)obs_data_get_int(settings, "bitrate");
		if (bitrate < 1)
			bitrate = 20000;

		// Cache the original bitrate per encoder so repeated
		// degrade/recover cycles never compound off a scaled value.
		int base = (int)obs_data_get_int(settings, "base_bitrate");
		if (base == 0) {
			obs_data_set_int(settings, "base_bitrate", bitrate);
			base = bitrate;
		}

		long long scaled = (long long)base * bitratePercent / 100LL;
		if (scaled < 1)
			scaled = 1;
		obs_data_set_int(settings, "bitrate", (int)scaled);

		int max_base = (int)obs_data_get_int(settings, "base_max_bitrate");
		if (max_base == 0)
			max_base = (int)obs_data_get_int(settings, "max_bitrate");
		if (max_base > 0) {
			obs_data_set_int(settings, "base_max_bitrate", max_base);
			long long scaled_max = (long long)max_base * bitratePercent / 100LL;
			if (scaled_max < scaled)
				scaled_max = scaled;
			obs_data_set_int(settings, "max_bitrate", (int)scaled_max);
		}

		obs_encoder_update(enc, settings);
	}
}

void BasicOutputHandler::RebindDegradeLayers(obs_output_t *output, int layers)
{
	const int full = (int)degradeEncoders.size();
	if (full <= 0)
		return;

	layers = std::clamp(layers, 1, full);

	// §R6: reducing layers drops the highest-resolution layer(s) and
	// keeps the lowest. Bound slots are ordered high (0) to low
	// (full-1), so keep the last `layers` and renumber them into slots
	// 0..layers-1 (the outputs build rid/track indices from slot order).
	const int start = full - layers;
	for (int i = 0; i < layers; i++)
		obs_output_set_video_encoder2(output, degradeEncoders[start + i].Get(), i);
	for (int i = layers; i < full; i++)
		obs_output_set_video_encoder2(output, nullptr, i);

	blog(LOG_INFO, "[degrade] rebound %d of %d encoder slot(s) (kept lowest-resolution)", layers, full);
}

BasicOutputHandler *CreateSimpleOutputHandler(OBSBasic *main)
{
	return new SimpleOutput(main);
}

BasicOutputHandler *CreateAdvancedOutputHandler(OBSBasic *main)
{
	return new AdvancedOutput(main);
}
