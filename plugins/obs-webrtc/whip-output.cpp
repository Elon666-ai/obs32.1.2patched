#include "whip-output.h"
#include "whip-utils.h"

#include <obs.hpp>
#include <util/ntp-clock.h>

#include <chrono>

#include <nlohmann/json.hpp>

#ifdef WHIP_DEGRADE_ACTIVE
#include "degrade-client.h"
#endif

/*
 * Sets the maximum size for a video fragment. Effective range is
 * 576-1470, with a lower value equating to more packets created,
 * but also better network compatability.
 */
static uint16_t MAX_VIDEO_FRAGMENT_SIZE = 1200;

/*
 * libdatachannel's ICE agent (unlike e.g. Pion, used on the MMX/server
 * side) reports PeerConnection::State::Disconnected the moment ICE
 * connectivity checks start failing, with no built-in tolerance for a
 * transient blip. Reacting to that immediately by tearing down and
 * rebuilding the whole WHIP session (DELETE + new POST + fresh ICE +
 * DTLS) is far more disruptive than the network hiccup that triggered
 * it - most Disconnected states self-recover within a few seconds.
 * This grace period gives the ICE agent time to recover on its own
 * before we give up; PeerConnection::State::Failed (which libdatachannel
 * only reports once ICE has actually given up) is still handled
 * immediately, with no grace period.
 */
const int signaling_media_id_length = 16;
const char signaling_media_id_valid_char[] = "0123456789"
					     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
					     "abcdefghijklmnopqrstuvwxyz";

const std::string user_agent = generate_user_agent();

const char *audio_mid = "0";
const uint8_t audio_payload_type = 111;

const char *video_mid = "1";
const uint8_t video_payload_type = 96;

// ~3 seconds of 8.5 Megabit video
const int video_nack_buffer_size = 4000;

const std::string rtpHeaderExtUriMid = "urn:ietf:params:rtp-hdrext:sdes:mid";
const std::string rtpHeaderExtUriRid = "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id";

WHIPOutput::WHIPOutput(obs_data_t *, obs_output_t *output)
	: output(output),
	  endpoint_url(),
	  bearer_token(),
	  resource_url(),
	  active_generation(0),
	  running(false),
	  start_stop_mutex(),
	  start_stop_thread(),
	  base_ssrc(generate_random_u32()),
	  peer_connection(nullptr),
	  audio_track(nullptr),
	  video_track(nullptr),
	  total_bytes_sent(0),
	  connect_time_ms(0),
	  start_time_ns(0),
	  last_audio_timestamp(0)
{
	// Declared at output creation so the frontend can connect before
	// streaming starts; emitted per scored sample from the quality
	// scorer's decode thread (see quality-score.cpp).
	signal_handler_add(obs_output_get_signal_handler(output),
			   "void quality_score(ptr output, float score, float psnr)");
}

WHIPOutput::~WHIPOutput()
{
	Stop();

	std::lock_guard<std::mutex> l(start_stop_mutex);
	if (start_stop_thread.joinable())
		start_stop_thread.join();
}

/*
 * Applies the encoder ROI configured on the WHIP service (if any) to
 * every simulcast layer encoder. The rectangle is specified at the
 * output (mix) resolution and scaled per layer, expanded outward so
 * integer rounding never shrinks the covered region. Two regions are
 * pushed per encoder: the quality-priority rectangle first, then a
 * full-frame background region - earlier regions win where they
 * overlap (see obs_encoder_add_roi docs), so the rectangle keeps its
 * priority and everything outside it gets the (negative) background
 * priority.
 */
void WHIPOutput::ApplyRoi()
{
	obs_service_t *service = obs_output_get_service(output);
	if (!service)
		return;

	OBSDataAutoRelease settings = obs_service_get_settings(service);

	// obs_context_data_init() never merges a service type's get_defaults()
	// into the actual runtime settings object (that only happens for the
	// scratch object obs_get_service_properties()/obs_service_defaults()
	// build to seed a properties dialog's displayed defaults), so
	// obs_data_get_double() below would silently return the library's
	// built-in 0.0 fallback instead of the intended default from
	// WHIPService::Defaults() whenever a user has never touched the
	// Manual ROI QP fields (Settings > Stream), making every ROI region a
	// zero-priority (i.e. no-op) no matter what the detector found.
	// Setting the same defaults again here, directly on this settings
	// object, makes the per-key fallback in obs_data_get_double() below
	// actually apply. Must stay in sync with WHIPService::Defaults() and
	// OBSBasicSettings::LoadStream1Settings()'s QP<->priority conversion
	// (6/-8 QP, i.e. +6/-8 out of 51 priority). Same gap applies to
	// the enable switches below, on a service that's never been through
	// the Settings dialog.
	obs_data_set_default_double(settings, "roi_priority", 6.0 / 51.0);
	obs_data_set_default_double(settings, "roi_bg_priority", -8.0 / 51.0);
	obs_data_set_default_bool(settings, "roi_enabled", false);
	obs_data_set_default_bool(settings, "detect_roi", true);

	const bool enabled = obs_data_get_bool(settings, "roi_enabled");

	// Master switch from Settings > Stream > Advanced Options. The
	// manually-configured rectangle ("roi_enabled", debug aid) takes
	// precedence over the detector when both are on.
	const bool detect_roi = obs_data_get_bool(settings, "detect_roi");

	video_t *video = obs_output_video(output);
	const struct video_output_info *voi = video ? video_output_get_info(video) : nullptr;
	const int64_t base_width = voi ? voi->width : 0;
	const int64_t base_height = voi ? voi->height : 0;

	const int64_t left = obs_data_get_int(settings, "roi_left");
	const int64_t top = obs_data_get_int(settings, "roi_top");
	const int64_t right = obs_data_get_int(settings, "roi_right");
	const int64_t bottom = obs_data_get_int(settings, "roi_bottom");
	const float priority = std::clamp((float)obs_data_get_double(settings, "roi_priority"), 0.0f, 1.0f);
	const float bg_priority = std::clamp((float)obs_data_get_double(settings, "roi_bg_priority"), -1.0f, 0.0f);

	const bool rect_valid = base_width > 0 && base_height > 0 && right > left && bottom > top;

	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		obs_encoder_t *encoder = obs_output_get_video_encoder2(output, idx);
		if (encoder == nullptr)
			break;

		// Encoders persist across output restarts, so regions from a
		// previous session must be cleared even when ROI is disabled.
		obs_encoder_clear_roi(encoder);
		if (!enabled || !rect_valid)
			continue;

		const int64_t enc_width = obs_encoder_get_width(encoder);
		const int64_t enc_height = obs_encoder_get_height(encoder);
		if (enc_width <= 0 || enc_height <= 0)
			continue;

		struct obs_encoder_roi region = {};
		region.left = (uint32_t)std::clamp<int64_t>(left * enc_width / base_width, 0, enc_width);
		region.top = (uint32_t)std::clamp<int64_t>(top * enc_height / base_height, 0, enc_height);
		region.right = (uint32_t)std::clamp<int64_t>((right * enc_width + base_width - 1) / base_width, 0,
							     enc_width);
		region.bottom = (uint32_t)std::clamp<int64_t>((bottom * enc_height + base_height - 1) / base_height, 0,
							      enc_height);
		region.priority = priority;

		bool ok = obs_encoder_add_roi(encoder, &region);

		if (ok && bg_priority < 0.0f) {
			struct obs_encoder_roi background = {};
			background.right = (uint32_t)enc_width;
			background.bottom = (uint32_t)enc_height;
			background.priority = bg_priority;
			ok = obs_encoder_add_roi(encoder, &background);
		}

		if (ok) {
			do_log(LOG_INFO,
			       "ROI applied to layer %u (%ux%u): rect %u,%u-%u,%u priority %.2f, "
			       "background priority %.2f",
			       idx, (uint32_t)enc_width, (uint32_t)enc_height, region.left, region.top, region.right,
			       region.bottom, priority, bg_priority);
		} else {
			do_log(LOG_WARNING,
			       "Failed to apply ROI to layer %u (%ux%u) - scaled region smaller than "
			       "16x16 or encoder lacks ROI support",
			       idx, (uint32_t)enc_width, (uint32_t)enc_height);
		}
	}

	if (detect_roi && !enabled) {
		do_log(LOG_INFO, "Ball/person detection ROI enabled - starting motion detector");
		motion_roi.Start(output, priority, bg_priority);
	} else {
		motion_roi.Stop();
		do_log(LOG_INFO, "Ball/person detection ROI is %s", detect_roi ? "superseded by manual ROI" : "disabled");
	}
}

bool WHIPOutput::Start()
{
	std::lock_guard<std::mutex> l(start_stop_mutex);
	const uint64_t generation = active_generation.fetch_add(1) + 1;

	// Only automatic reconnect attempts (triggered by libobs after a
	// disconnect) should preserve fail_since_ns/using_backup across
	// Start() calls. A deliberate user-initiated start (Stop then Start,
	// or the very first start) should always retry the primary server.
	if (!obs_output_reconnecting(output)) {
		using_backup = false;
		fail_since_set = false;
		reconnect_attempt = 0;
	}

	if (!obs_output_can_begin_data_capture(output, 0))
		return false;
	if (!obs_output_initialize_encoders(output, 0))
		return false;

	// Join the previous session's StopThread before touching
	// videoLayerStates: StopThread() clears the map as its last act, so
	// populating it first would race a still-running teardown and leave
	// Data() dropping every video packet as "stale" for the whole session.
	if (start_stop_thread.joinable())
		start_stop_thread.join();

	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		auto encoder = obs_output_get_video_encoder2(output, idx);
		if (encoder == nullptr) {
			break;
		}

		auto v = std::make_shared<videoLayerState>();
		// base_ssrc is ssrc for audio track. We do `+ 1` for the video, then idx for each Simulcast layer.
		v->ssrc = base_ssrc + 1 + idx;
		v->rid = std::to_string(idx);
		videoLayerStates[encoder] = v;
	}

	ApplyRoi();

	{
		obs_service_t *service = obs_output_get_service(output);
		OBSDataAutoRelease service_settings = service ? obs_service_get_settings(service) : nullptr;
		if (service_settings && obs_data_get_bool(service_settings, "quality_score"))
			quality_scorer.Start(output);
	}

#ifdef WHIP_DEGRADE_ACTIVE
	// Kick off the mmx degrade-channel WS connection as early as
	// possible (before the WHIP/RTC connection itself even starts
	// negotiating in StartThread below), rather than waiting until
	// after data capture has begun - the WS connection and the WHIP
	// connection are otherwise unrelated, so there's no reason to
	// serialize them. Simulcast layer count is decided purely by local
	// config and is never touched by degrade-client regardless of when
	// this is called (see the comment on TargetState in
	// degrade-client.h); this just gets bitrate-degrade adaptation
	// wired up sooner.
	WsDegradeClient::Instance().RegisterOutput(output);
#endif

	start_stop_thread = std::thread(&WHIPOutput::StartThread, this, generation);

	return true;
}

void WHIPOutput::Stop(bool signal)
{
	// Whatever the reason we're stopping (user request, Failed state, or
	// the grace timer below giving up), any pending Disconnected grace
	// timer and the liveness watchdog are now moot.
	CancelDisconnectGraceTimer();
	StopWatchdog();

	motion_roi.Stop();
	quality_scorer.Stop();

#ifdef WHIP_DEGRADE_ACTIVE
	WsDegradeClient::Instance().UnregisterOutput();
#endif

	std::lock_guard<std::mutex> l(start_stop_mutex);
	const uint64_t generation = active_generation.load();
	std::string resourceURL;
	{
		std::lock_guard<std::mutex> rl(resource_mutex);
		resourceURL = resource_url;
	}
	if (start_stop_thread.joinable())
		start_stop_thread.join();

	start_stop_thread = std::thread(&WHIPOutput::StopThread, this, signal, generation, resourceURL);
}

void WHIPOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		// Diagnostic-only addition: this branch silently calls
		// Stop(false) -> OBS_OUTPUT_ENCODE_ERROR, which can_reconnect()
		// in libobs/obs-output.c does NOT treat as reconnectable (only
		// OBS_OUTPUT_DISCONNECTED is), and which - if it lands right
		// after a just-succeeded reconnect (reconnecting flag already
		// cleared by begin_delayed_capture) - looks to the user like
		// the stream just silently died with no error message at all.
		do_log(LOG_WARNING, "Data() called with null packet - stopping (no reconnect, OBS_OUTPUT_ENCODE_ERROR, was_reconnecting=%d)",
		       obs_output_reconnecting(output));
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	// Hold the shared (reader) lock for the whole function, not just
	// while snapshotting the shared_ptrs: StartThread()/StopThread()
	// take the exclusive (writer) lock around peer_connection->close(),
	// so this also blocks a close() from running concurrently with the
	// track->send() calls below. Without that, close() can tear down
	// the transport out from under a send() that libdatachannel's RTC
	// worker thread is still in the middle of delivering - a deeper
	// race than the shared_ptr one, previously seen crashing the RTC
	// worker thread inside datachannel.dll. Root-caused an OBS
	// crash/hang after ~11h of overnight streaming.
	std::shared_lock<std::shared_mutex> lk(tracks_mutex);
	if (!running.load() || teardown_in_progress.load())
		return;
	std::shared_ptr<rtc::Track> local_audio_track = audio_track;
	std::shared_ptr<rtc::Track> local_video_track = video_track;
	std::shared_ptr<rtc::DataChannel> local_timestamp_channel = timestamp_channel;
	std::shared_ptr<rtc::RtcpSrReporter> local_audio_sr_reporter = audio_sr_reporter;
	std::shared_ptr<rtc::RtcpSrReporter> local_video_sr_reporter = video_sr_reporter;

	if (local_audio_track && packet->type == OBS_ENCODER_AUDIO) {
		int64_t duration = packet->dts_usec - last_audio_timestamp;
		Send(packet->data, packet->size, duration, local_audio_track, local_audio_sr_reporter);
		last_audio_timestamp = packet->dts_usec;
	} else if (local_video_track && packet->type == OBS_ENCODER_VIDEO) {
		auto rtp_config = local_video_sr_reporter->rtpConfig;
		auto videoLayerState = videoLayerStates[packet->encoder];
		if (videoLayerState == nullptr) {
			// Stale encoder packet in flight after reconnect:
			// Start() rebuilt the map with new encoders, but
			// old encoder packets can still arrive. Drop
			// silently instead of killing the stream.
			do_log(LOG_DEBUG,
			       "Data() video packet encoder=%p not found in videoLayerStates (size=%zu) - dropping stale packet",
			       (void *)packet->encoder, videoLayerStates.size());
			return;
		}

		quality_scorer.OnPacket(packet);

		rtp_config->sequenceNumber = videoLayerState->sequenceNumber;
		rtp_config->ssrc = videoLayerState->ssrc;
		rtp_config->rid = videoLayerState->rid;
		rtp_config->timestamp = videoLayerState->rtpTimestamp;
		int64_t duration = packet->dts_usec - videoLayerState->lastVideoTimestamp;

		// frame_no is the starting RTP sequence number for this frame's
		// first packet (it wraps at 65536 same as the RTP field itself).
		// See docs/obs-abs-timestamp-protocol.md.
		if (local_timestamp_channel && local_timestamp_channel->isOpen()) {
			nlohmann::json ts_msg = {
				{"frame_no", videoLayerState->sequenceNumber},
				{"timestamp", ntp_clock_now_ms()},
				{"rid", videoLayerState->rid},
			};
			try {
				local_timestamp_channel->send(ts_msg.dump());
			} catch (const std::exception &e) {
				do_log(LOG_DEBUG, "timestamp_channel send failed: %s", e.what());
			}
		}

		Send(packet->data, packet->size, duration, local_video_track, local_video_sr_reporter);

		videoLayerState->sequenceNumber = rtp_config->sequenceNumber;
		videoLayerState->lastVideoTimestamp = packet->dts_usec;
		videoLayerState->rtpTimestamp = rtp_config->timestamp;
	}
}

void WHIPOutput::ConfigureAudioTrack(std::string media_stream_id, std::string cname)
{
	if (!obs_output_get_audio_encoder(output, 0)) {
		do_log(LOG_DEBUG, "Not configuring audio track: Audio encoder not assigned");
		return;
	}

	auto media_stream_track_id = std::string(media_stream_id + "-audio");

	uint32_t ssrc = base_ssrc;

	rtc::Description::Audio audio_description(audio_mid, rtc::Description::Direction::SendOnly);
	audio_description.addOpusCodec(audio_payload_type);
	audio_description.addSSRC(ssrc, cname, media_stream_id, media_stream_track_id);
	auto new_audio_track = peer_connection->addTrack(audio_description);

	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, audio_payload_type,
									rtc::OpusRtpPacketizer::DefaultClockRate);
	auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);
	auto new_audio_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();

	packetizer->addToChain(new_audio_sr_reporter);
	packetizer->addToChain(nack_responder);
	new_audio_track->setMediaHandler(packetizer);

	std::unique_lock<std::shared_mutex> lk(tracks_mutex);
	audio_track = new_audio_track;
	audio_sr_reporter = new_audio_sr_reporter;
}

void WHIPOutput::ConfigureVideoTrack(std::string media_stream_id, std::string cname)
{
	if (!obs_output_get_video_encoder(output)) {
		do_log(LOG_DEBUG, "Not configuring video track: Video encoder not assigned");
		return;
	}

	auto media_stream_track_id = std::string(media_stream_id + "-video");
	std::shared_ptr<rtc::RtpPacketizer> packetizer;

	// More predictable SSRC values between audio and video
	uint32_t ssrc = base_ssrc + 1;

	rtc::Description::Video video_description(video_mid, rtc::Description::Direction::SendOnly);
	video_description.addSSRC(ssrc, cname, media_stream_id, media_stream_track_id);

	video_description.addExtMap(rtc::Description::Entry::ExtMap(1, rtpHeaderExtUriMid));
	video_description.addExtMap(rtc::Description::Entry::ExtMap(2, rtpHeaderExtUriRid));

	if (videoLayerStates.size() >= 2) {
		std::vector<std::pair<int, std::string>> sortedRids;

		for (const auto &[encoder, state] : videoLayerStates) {
			sortedRids.push_back({std::stoi(state->rid), state->rid});
		}

		std::sort(sortedRids.begin(), sortedRids.end(),
			  [](const auto &a, const auto &b) { return a.first < b.first; });

		for (const auto &[_, rid] : sortedRids) {
			video_description.addRid(rid);
		}
	}

	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, video_payload_type,
#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR > 22 || RTC_VERSION_MAJOR > 0
									rtc::H264RtpPacketizer::ClockRate);
#else
									rtc::H264RtpPacketizer::defaultClockRate);
#endif

	rtp_config->midId = 1;
	rtp_config->ridId = 2;
	rtp_config->mid = video_mid;

	const obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0);
	if (!encoder)
		return;

	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	auto video_bitrate = (int)obs_data_get_int(settings, "bitrate");

	const char *codec = obs_encoder_get_codec(encoder);
	if (strcmp("h264", codec) == 0) {
		video_description.addH264Codec(video_payload_type);
		packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::H264RtpPacketizer::Separator::StartSequence,
								      rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
#ifdef ENABLE_HEVC
	} else if (strcmp("hevc", codec) == 0) {
		video_description.addH265Codec(video_payload_type);
		packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::H265RtpPacketizer::Separator::StartSequence,
								      rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
#endif
	} else if (strcmp("av1", codec) == 0) {
		video_description.addAV1Codec(video_payload_type);
		packetizer = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit,
								     rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
	} else {
		do_log(LOG_ERROR, "Video codec not supported: %s", codec);
		return;
	}

	auto new_video_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	packetizer->addToChain(new_video_sr_reporter);
	packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(video_nack_buffer_size));

	if (video_bitrate != 0) {
		packetizer->addToChain(std::make_shared<rtc::PacingHandler>(static_cast<double>(video_bitrate * 10000),
									    std::chrono::milliseconds(5)));
	}

	auto new_video_track = peer_connection->addTrack(video_description);
	new_video_track->setMediaHandler(packetizer);

	std::unique_lock<std::shared_mutex> lk(tracks_mutex);
	video_track = new_video_track;
	video_sr_reporter = new_video_sr_reporter;
}

/**
 * @brief Store connect info provided by the service.
 *
 * @return bool
 */
bool WHIPOutput::Init()
{
	OBSDataAutoRelease output_settings = obs_output_get_settings(output);
	disconnect_grace_sec = (int)obs_data_get_int(output_settings, "whip_disconnect_grace_sec");
	reconnect_backoff_sec = (int)obs_data_get_int(output_settings, "whip_reconnect_backoff_sec");
	if (!obs_data_has_user_value(output_settings, "whip_disconnect_grace_sec"))
		disconnect_grace_sec = 10;
	if (!obs_data_has_user_value(output_settings, "whip_reconnect_backoff_sec"))
		reconnect_backoff_sec = 3;
	if (disconnect_grace_sec < 0)
		disconnect_grace_sec = 0;
	if (reconnect_backoff_sec < 0)
		reconnect_backoff_sec = 0;

	watchdog_interval_sec = (int)obs_data_get_int(output_settings, "whip_watchdog_interval_sec");
	watchdog_stall_sec = (int)obs_data_get_int(output_settings, "whip_watchdog_stall_sec");
	if (!obs_data_has_user_value(output_settings, "whip_watchdog_interval_sec"))
		watchdog_interval_sec = 10;
	if (!obs_data_has_user_value(output_settings, "whip_watchdog_stall_sec"))
		watchdog_stall_sec = 30;
	if (watchdog_interval_sec < 0)
		watchdog_interval_sec = 0;
	// A stall threshold below the poll interval would fire on the very
	// first tick; keep it at least one interval so a single slow poll
	// can never be mistaken for a stalled stream.
	if (watchdog_stall_sec > 0 && watchdog_stall_sec < watchdog_interval_sec)
		watchdog_stall_sec = watchdog_interval_sec;

	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	const char *server_url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *token = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN);

	// Check for fallback: if primary has been failing >30s and a backup
	// is configured, switch. Failing on backup >30s → switch back. The
	// bearer token authenticates either one.
	const char *backup_url_c = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_BACKUP_SERVER);

	if (ShouldFallback(backup_url_c ? backup_url_c : "")) {
		using_backup = !using_backup;
		fail_since_set = false;
		do_log(LOG_INFO, "Fallback: switching to %s server", using_backup ? "backup" : "primary");
	}

	if (using_backup && backup_url_c && backup_url_c[0]) {
		endpoint_url = backup_url_c;
	} else {
		endpoint_url = server_url ? server_url : "";
		using_backup = false;
	}

	bearer_token = token ? token : "";
	// If the user didn't fill in a Bearer Token, fall back to
	// WHIP_WS_SECRET so publish auth still passes against a server
	// configured with that shared secret, same as before ppcenter-issued
	// per-session tokens existed. Falls back further to a hardcoded
	// default so a fresh checkout works against a stock dev/test server
	// with zero configuration.
	if (bearer_token.empty()) {
		const char *env_secret = getenv("WHIP_WS_SECRET");
		if (env_secret && env_secret[0])
			bearer_token = env_secret;
		else
			bearer_token = "de4e53fe0b4565358cf5b47c89cc6dbbc0f902c62e4c2952";
	}

	if (endpoint_url.empty()) {
		obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	return true;
}

/**
 * @brief Set up the PeerConnection and media tracks.
 *
 * @return bool
 */
bool WHIPOutput::Setup(uint64_t generation)
{
	rtc::Configuration cfg;

#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR > 20 || RTC_VERSION_MAJOR > 0
	cfg.disableAutoGathering = true;
#endif

	peer_connection = std::make_shared<rtc::PeerConnection>(cfg);

	peer_connection->onStateChange([this, generation](rtc::PeerConnection::State state) {
		if (!IsActiveGeneration(generation)) {
			do_log(LOG_DEBUG, "[WHIP generation=%llu] ignoring stale PeerConnection state change: %d",
			       static_cast<unsigned long long>(generation), static_cast<int>(state));
			return;
		}
		switch (state) {
		case rtc::PeerConnection::State::New:
			do_log(LOG_INFO, "PeerConnection state is now: New");
			break;
		case rtc::PeerConnection::State::Connecting:
			do_log(LOG_INFO, "PeerConnection state is now: Connecting");
			start_time_ns = os_gettime_ns();
			break;
		case rtc::PeerConnection::State::Connected:
			do_log(LOG_INFO, "PeerConnection state is now: Connected");
			connect_time_ms = (int)((os_gettime_ns() - start_time_ns) / 1000000.0);
			do_log(LOG_INFO, "Connect time: %dms", connect_time_ms.load());
			MarkConnected();
			CancelDisconnectGraceTimer();
			break;
		case rtc::PeerConnection::State::Disconnected:
			do_log(LOG_INFO,
			       "PeerConnection state is now: Disconnected - waiting up to %ds for it to recover before tearing down",
			       disconnect_grace_sec);
			StartDisconnectGraceTimer(generation);
			break;
		case rtc::PeerConnection::State::Failed:
			do_log(LOG_INFO, "PeerConnection state is now: Failed");
			MarkDisconnected();
			PrepareReconnect();
			Stop(false);
			// OBS_OUTPUT_DISCONNECTED (not OBS_OUTPUT_ERROR) so
			// libobs's own can_reconnect() treats this as a normal,
			// reconnectable network drop: it silently retries via
			// output_reconnect() instead of immediately ending data
			// capture and popping the "connection failed" dialog in
			// front of the user for something WHIP is already about
			// to recover from on its own (see the grace-period retry
			// this output does independently, and the Reconnect
			// setting in Settings > Output, which is what actually
			// gates whether libobs's reconnect kicks in here). If
			// reconnection attempts are ever exhausted, libobs signals
			// stop with this same code, and the resulting dialog text
			// ("Disconnected from server") is more accurate for this
			// case than the generic "unexpected error" text
			// OBS_OUTPUT_ERROR would have shown anyway.
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
			break;
		case rtc::PeerConnection::State::Closed:
			do_log(LOG_INFO, "PeerConnection state is now: Closed");
			// A remote close can leave application-side tracks looking open
			// while libdatachannel is destroying their transport. Invalidate
			// them immediately so encoder threads cannot enter send() during
			// that teardown window. Explicit StopThread() handles its own
			// close under teardown_in_progress.
			if (teardown_in_progress.load())
				break;
			running = false;
			{
				std::unique_lock<std::shared_mutex> lk(tracks_mutex);
				audio_track = nullptr;
				video_track = nullptr;
				timestamp_channel = nullptr;
				audio_sr_reporter = nullptr;
				video_sr_reporter = nullptr;
			}
			break;
		}
	});

	std::string media_stream_id, cname;
	media_stream_id.reserve(signaling_media_id_length);
	cname.reserve(signaling_media_id_length);

	for (int i = 0; i < signaling_media_id_length; ++i) {
		media_stream_id += signaling_media_id_valid_char[rand() % (sizeof(signaling_media_id_valid_char) - 1)];

		cname += signaling_media_id_valid_char[rand() % (sizeof(signaling_media_id_valid_char) - 1)];
	}

	ConfigureAudioTrack(media_stream_id, cname);
	ConfigureVideoTrack(media_stream_id, cname);

	// Side channel for per-frame push timestamps; see
	// docs/obs-abs-timestamp-protocol.md. Best-effort only: if the
	// remote end doesn't accept it, sends below are just no-ops since
	// timestamp_channel never reports isOpen(). Must be created AFTER
	// addTrack (Configure*Track) calls so libdatachannel includes
	// both the DataChannel and the media m-lines in the SDP offer.
	auto new_timestamp_channel = peer_connection->createDataChannel("obs-timestamp");
	{
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		timestamp_channel = new_timestamp_channel;
	}

	peer_connection->setLocalDescription();

	return true;
}

// Given a Link header extract URL/Username/Credential and create rtc::IceServer
// <turn:turn.example.net>; username="user"; credential="myPassword";
//
// https://www.ietf.org/archive/id/draft-ietf-wish-whip-13.html#section-4.4
void WHIPOutput::ParseLinkHeader(std::string val, std::vector<rtc::IceServer> &iceServers)
{
	std::string url, username, password;

	auto extractUrl = [](std::string input) -> std::string {
		auto head = input.find("<") + 1;
		auto tail = input.find(">");

		if (head == std::string::npos || tail == std::string::npos) {
			return "";
		}
		return input.substr(head, tail - head);
	};

	auto extractValue = [](std::string input) -> std::string {
		auto head = input.find("\"") + 1;
		auto tail = input.find_last_of("\"");

		if (head == std::string::npos || tail == std::string::npos) {
			return "";
		}
		return input.substr(head, tail - head);
	};

	while (true) {
		std::string token = val;
		auto pos = token.find(";");
		if (pos != std::string::npos) {
			token = val.substr(0, pos);
		}

		if ((token.find("<stun:", 0) == 0) || (token.find("<turn:", 0) == 0)) {
			url = extractUrl(token);
		} else if (token.find("username=") != std::string::npos) {
			username = extractValue(token);
		} else if (token.find("credential=") != std::string::npos) {
			password = extractValue(token);
		}

		if (pos == std::string::npos) {
			break;
		}
		val.erase(0, pos + 1);
	}

	try {
		auto iceServer = rtc::IceServer(url);
		iceServer.username = username;
		iceServer.password = password;
		iceServers.push_back(iceServer);
	} catch (const std::invalid_argument &err) {
		do_log(LOG_WARNING, "Failed to construct ICE Server from %s: %s", val.c_str(), err.what());
	}
}

bool WHIPOutput::Connect(uint64_t generation, std::string &attemptResourceURL)
{
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/sdp");
	if (!bearer_token.empty()) {
		auto bearer_token_header = std::string("Authorization: Bearer ") + bearer_token;
		headers = curl_slist_append(headers, bearer_token_header.c_str());
	}

	std::string read_buffer;
	std::vector<std::string> http_headers;

	auto offer_sdp = std::string(peer_connection->localDescription().value());

#ifdef DEBUG_SDP
	do_log(LOG_DEBUG, "Offer SDP:\n%s", offer_sdp.c_str());
#endif

	// Add user-agent to our requests
	headers = curl_slist_append(headers, user_agent.c_str());

	char error_buffer[CURL_ERROR_SIZE] = {};

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_writefunction);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&read_buffer);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header_function);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *)&http_headers);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, endpoint_url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, offer_sdp.c_str());
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 1L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error_buffer);

	auto doCleanup = [&](bool connectFailed) {
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
		if (connectFailed && IsActiveGeneration(generation)) {
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
		}
	};

	auto displayError = [&](const char *what, const char *errorMessage) {
		struct dstr error_message;
		dstr_init_copy(&error_message, obs_module_text(errorMessage));
		dstr_replace(&error_message, "%1", what);
		obs_output_set_last_error(output, error_message.array);
		dstr_free(&error_message);
	};

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_ERROR, "Connect failed: %s", error_buffer[0] ? error_buffer : curl_easy_strerror(res));
		doCleanup(true);
		return false;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 201) {
		do_log(LOG_ERROR, "Connect failed: HTTP endpoint returned response code %ld", response_code);
		doCleanup(false);
		if (IsActiveGeneration(generation)) {
			// 401/403/404 mean the server rejected this specific
			// stream (bad key/path/permissions, e.g. mmx's
			// publish whitelist) rather than a transient network
			// problem - retrying on libobs's reconnect timer will
			// just get the identical rejection every time. Signal
			// OBS_OUTPUT_INVALID_STREAM (not reconnectable, see
			// can_reconnect() in libobs/obs-output.c) so the user
			// gets a clear "invalid stream key/path" dialog and
			// streaming actually stops, instead of retrying
			// forever with no visible error (as OBS_OUTPUT_
			// DISCONNECTED would do). Other codes (5xx, etc.) keep
			// the existing reconnectable behavior.
			bool permanent = response_code == 401 || response_code == 403 || response_code == 404;
			obs_output_signal_stop(output, permanent ? OBS_OUTPUT_INVALID_STREAM
								  : OBS_OUTPUT_DISCONNECTED);
		}
		return false;
	}

	if (read_buffer.empty()) {
		do_log(LOG_ERROR, "Connect failed: No data returned from HTTP endpoint request");
		doCleanup(true);
		return false;
	}

	long redirect_count = 0;
	curl_easy_getinfo(c, CURLINFO_REDIRECT_COUNT, &redirect_count);

	std::string last_location_header;
	size_t location_header_count = 0;
	for (auto &http_header : http_headers) {
		auto value = value_for_header("location", http_header);
		if (value.empty())
			continue;

		location_header_count++;
		last_location_header = value;
	}

	if (location_header_count < static_cast<size_t>(redirect_count) + 1) {
		do_log(LOG_ERROR, "WHIP server did not provide a resource URL via the Location header");
		doCleanup(true);
		return false;
	}

	CURLU *url_builder = curl_url();

	// Parse Link headers to extract STUN/TURN server configuration URLs
	std::vector<rtc::IceServer> iceServers;
	for (auto &http_header : http_headers) {
		auto value = value_for_header("link", http_header);
		if (value.empty())
			continue;

		// Parse multiple links separated by ','
		for (auto end = value.find(","); end != std::string::npos; end = value.find(",")) {
			this->ParseLinkHeader(value.substr(0, end), iceServers);
			value = value.substr(end + 1);
		}
		this->ParseLinkHeader(value, iceServers);
	}

	// If Location header doesn't start with `http` it is a relative URL.
	// Construct a absolute URL using the host of the effective URL
	if (last_location_header.find("http") != 0) {
		char *effective_url = nullptr;
		curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective_url);
		if (effective_url == nullptr) {
			do_log(LOG_ERROR, "Failed to build Resource URL");
			doCleanup(true);
			return false;
		}

		curl_url_set(url_builder, CURLUPART_URL, effective_url, 0);
		curl_url_set(url_builder, CURLUPART_PATH, last_location_header.c_str(), 0);
		curl_url_set(url_builder, CURLUPART_QUERY, "", 0);
	} else {
		curl_url_set(url_builder, CURLUPART_URL, last_location_header.c_str(), 0);
	}

	char *url = nullptr;
	CURLUcode rc = curl_url_get(url_builder, CURLUPART_URL, &url, CURLU_NO_DEFAULT_PORT);
	if (rc) {
		do_log(LOG_ERROR, "WHIP server provided a invalid resource URL via the Location header");
		doCleanup(true);
		return false;
	}

	attemptResourceURL = url;
	if (IsActiveGeneration(generation)) {
		std::lock_guard<std::mutex> rl(resource_mutex);
		resource_url = attemptResourceURL;
	}
	curl_free(url);
	do_log(LOG_DEBUG, "[WHIP generation=%llu] WHIP Resource URL is: %s",
	       static_cast<unsigned long long>(generation), attemptResourceURL.c_str());
	curl_url_cleanup(url_builder);

#ifdef DEBUG_SDP
	do_log(LOG_DEBUG, "Answer SDP:\n%s", read_buffer.c_str());
#endif

	auto response = std::string(read_buffer);
	response.erase(0, response.find("v=0"));

	// If we are sending multiple layers assert that the remote accepted them all
	if (videoLayerStates.size() != 1) {
		auto layersAccepted = simulcast_layers_in_answer(response);
		if (videoLayerStates.size() != layersAccepted) {
			do_log(LOG_ERROR, "WHIP only accepted %lu layers", layersAccepted);
			displayError(std::to_string(layersAccepted).c_str(), "Error.SimulcastLayersRejected");
			doCleanup(true);
			return false;
		}
	}

	rtc::Description answer(response, "answer");
	try {
		peer_connection->setRemoteDescription(answer);
	} catch (const std::invalid_argument &err) {
		do_log(LOG_ERROR, "WHIP server responded with invalid SDP: %s", err.what());
		doCleanup(true);
		if (IsActiveGeneration(generation)) {
			struct dstr error_message;
			dstr_init_copy(&error_message, obs_module_text("Error.InvalidSDP"));
			dstr_replace(&error_message, "%1", err.what());
			obs_output_set_last_error(output, error_message.array);
			dstr_free(&error_message);
		}
		return false;
	} catch (const std::exception &err) {
		do_log(LOG_ERROR, "Failed to set remote description: %s", err.what());
		doCleanup(true);
		if (IsActiveGeneration(generation)) {
			struct dstr error_message;
			dstr_init_copy(&error_message, obs_module_text("Error.NoRemoteDescription"));
			dstr_replace(&error_message, "%1", err.what());
			obs_output_set_last_error(output, error_message.array);
			dstr_free(&error_message);
		}
		return false;
	}
	doCleanup(false);

#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR > 20 || RTC_VERSION_MAJOR > 0
	peer_connection->gatherLocalCandidates(iceServers);
#endif

	return true;
}

void WHIPOutput::StartThread(uint64_t generation)
{
	if (!Init())
		return;

	if (!Setup(generation))
		return;

	std::string attemptResourceURL;
	if (!Connect(generation, attemptResourceURL)) {
		MarkDisconnected();
		{
			teardown_in_progress = true;
			std::unique_lock<std::shared_mutex> lk(tracks_mutex);
			peer_connection->close();
			peer_connection = nullptr;
			audio_track = nullptr;
			video_track = nullptr;
			timestamp_channel = nullptr;
			audio_sr_reporter = nullptr;
			video_sr_reporter = nullptr;
			teardown_in_progress = false;
		}
		return;
	}

	if (!IsActiveGeneration(generation)) {
		do_log(LOG_WARNING, "[WHIP generation=%llu] stale attempt became connected; deleting only its own resource",
		       static_cast<unsigned long long>(generation));
		{
			teardown_in_progress = true;
			std::unique_lock<std::shared_mutex> lk(tracks_mutex);
			if (peer_connection)
				peer_connection->close();
			peer_connection = nullptr;
			audio_track = nullptr;
			video_track = nullptr;
			timestamp_channel = nullptr;
			audio_sr_reporter = nullptr;
			video_sr_reporter = nullptr;
			teardown_in_progress = false;
		}
		SendDelete(attemptResourceURL, generation, "stale_generation_after_connect");
		return;
	}

	obs_output_begin_data_capture(output, 0);
	running = true;

	// Started only after data capture begins: before this point a flat
	// byte counter is normal, and the connect path has its own timeouts.
	StartWatchdog(generation);
}

void WHIPOutput::SendDelete(const std::string &resourceURL, uint64_t generation, const char *reason)
{
	if (resourceURL.empty()) {
		do_log(LOG_DEBUG, "[WHIP generation=%llu] No resource URL available, not sending DELETE (%s)",
		       static_cast<unsigned long long>(generation), reason ? reason : "unknown");
		return;
	}

	struct curl_slist *headers = NULL;
	if (!bearer_token.empty()) {
		auto bearer_token_header = std::string("Authorization: Bearer ") + bearer_token;
		headers = curl_slist_append(headers, bearer_token_header.c_str());
	}

	// Add user-agent to our requests
	headers = curl_slist_append(headers, user_agent.c_str());

	char error_buffer[CURL_ERROR_SIZE] = {};

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, resourceURL.c_str());
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error_buffer);

	auto doCleanup = [&]() {
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
	};

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_WARNING, "[WHIP generation=%llu] DELETE request for resource URL failed (%s): %s",
		       static_cast<unsigned long long>(generation), reason ? reason : "unknown",
		       error_buffer[0] ? error_buffer : curl_easy_strerror(res));
		doCleanup();
		return;
	}

	long response_code;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 200) {
		do_log(LOG_WARNING, "[WHIP generation=%llu] DELETE request for resource URL failed (%s). HTTP Code: %ld",
		       static_cast<unsigned long long>(generation), reason ? reason : "unknown", response_code);
		doCleanup();
		return;
	}

	do_log(LOG_DEBUG, "[WHIP generation=%llu] Successfully performed DELETE request for resource URL (%s)",
	       static_cast<unsigned long long>(generation), reason ? reason : "unknown");
	if (IsActiveGeneration(generation)) {
		std::lock_guard<std::mutex> rl(resource_mutex);
		if (resource_url == resourceURL)
			resource_url.clear();
	}
	doCleanup();
}

void WHIPOutput::StopThread(bool signal, uint64_t generation, std::string resourceURL)
{
	if (!resourceURL.empty()) {
		do_log(LOG_DEBUG, "[WHIP generation=%llu] StopThread reason=%s resource=%s",
		       static_cast<unsigned long long>(generation), signal ? "user_or_ui" : "internal_or_reconnect",
		       resourceURL.c_str());
	}
	if (peer_connection != nullptr) {
		teardown_in_progress = true;
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		peer_connection->close();
		peer_connection = nullptr;
		audio_track = nullptr;
		video_track = nullptr;
		timestamp_channel = nullptr;
		audio_sr_reporter = nullptr;
		video_sr_reporter = nullptr;
		teardown_in_progress = false;
	}

	SendDelete(resourceURL, generation, signal ? "stop_thread" : "reconnect_or_error");

	/*
	 * "signal" exists because we have to preserve the "running" state
	 * across reconnect attempts. If we don't emit a signal if
	 * something calls obs_output_stop() and it's reconnecting, you'll
	 * desync the UI, as the output will be "stopped" and not
	 * "reconnecting", but the "stop" signal will have never been
	 * emitted.
	 */
	if (running && signal) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);
		running = false;
	}

	total_bytes_sent = 0;
	connect_time_ms = 0;
	start_time_ns = 0;
	last_audio_timestamp = 0;
	videoLayerStates.clear();
}

bool WHIPOutput::IsActiveGeneration(uint64_t generation) const
{
	return active_generation.load() == generation;
}

// -------------------------------------------------------------------
// Fallback helpers
// -------------------------------------------------------------------
void WHIPOutput::MarkDisconnected()
{
	if (!fail_since_set) {
		fail_since_ns = os_gettime_ns();
		fail_since_set = true;
		do_log(LOG_INFO,
		       "Failover: recording disconnect time, using_backup=%d",
		       (int)using_backup);
	}
}

void WHIPOutput::MarkConnected()
{
	reconnect_attempt = 0;
	if (fail_since_set) {
		int64_t elapsed_ns = os_gettime_ns() - fail_since_ns;
		do_log(LOG_INFO,
		       "Failover: connected after %.1fs on %s server, resetting timer",
		       (double)elapsed_ns / 1e9,
		       using_backup ? "backup" : "primary");
	}
	fail_since_set = false;
	fail_since_ns = 0;
}

void WHIPOutput::PrepareReconnect()
{
	// fetch_add returns the *previous* value, so the first attempt must be
	// scaled by attempt + 1. Scaling by the raw fetch_add result made the
	// first reconnect wait 0s, which let libobs's reconnect thread call
	// Start() before the previous session's end_data_capture_thread had
	// cleared the output's "active" flag; Start() then failed on
	// obs_output_can_begin_data_capture() and the output was left stuck
	// in the reconnecting state (streaming at 0 bitrate) until manually
	// restarted. reconnect_thread() in libobs now reschedules a failed
	// start as well, but there is no reason to schedule a doomed attempt
	// in the first place.
	const int attempt = reconnect_attempt.fetch_add(1) + 1;
	const int delay_sec = reconnect_backoff_sec * attempt;
	obs_output_set_reconnect_delay(output, delay_sec * 1000);
	do_log(LOG_INFO, "WHIP reconnect attempt %d: waiting %ds before next session", attempt, delay_sec);
}

bool WHIPOutput::ShouldFallback(const std::string &backup_url)
{
	if (!fail_since_set)
		return false;
	if (backup_url.empty())
		return false;

	int64_t elapsed_ns = os_gettime_ns() - fail_since_ns;
	if (elapsed_ns < 30LL * 1000000000LL)
		return false;

	do_log(LOG_INFO,
	       "Failover: %.1fs elapsed, triggering switch from %s",
	       (double)elapsed_ns / 1e9,
	       using_backup ? "backup" : "primary");
	return true;
}

// -------------------------------------------------------------------
// Disconnected grace period (configured through OBS Advanced settings).
// -------------------------------------------------------------------
void WHIPOutput::StartDisconnectGraceTimer(uint64_t generation)
{
	// Cancel/join any previous timer first; this always runs on the
	// PeerConnection callback thread, never on disconnect_grace_thread
	// itself, so the join below is safe.
	CancelDisconnectGraceTimer();

	{
		std::lock_guard<std::mutex> lk(disconnect_grace_mutex);
		disconnect_grace_cancel = false;
	}

	disconnect_grace_thread = std::thread([this, generation]() {
		std::unique_lock<std::mutex> lk(disconnect_grace_mutex);
		bool cancelled = disconnect_grace_cv.wait_for(lk, std::chrono::seconds(disconnect_grace_sec),
							      [this]() { return disconnect_grace_cancel; });
		lk.unlock();

		if (cancelled)
			return;
		if (!IsActiveGeneration(generation))
			return;

		do_log(LOG_INFO,
		       "PeerConnection stayed Disconnected for %ds without recovering, tearing down and reconnecting",
		       disconnect_grace_sec);
		MarkDisconnected();
		PrepareReconnect();
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
	});
}

// -------------------------------------------------------------------
// Periodic liveness watchdog.
// -------------------------------------------------------------------
//
// The PeerConnection state machine only reports transport-level faults:
// it says nothing when a session is nominally Connected and "running"
// but has silently stopped pushing bytes. That gap is exactly how a
// stuck output can sit there for hours looking healthy in the UI while
// the far end receives nothing, with the log showing no clue beyond the
// absence of new lines - which is not something anyone notices at 2am.
//
// So poll instead of trusting state transitions: every
// whip_watchdog_interval_sec, log a one-line heartbeat with the byte
// counter, and if the counter has not advanced for whip_watchdog_stall_sec
// while we still believe we are running, treat it as a dead session and
// hand it to the same teardown path a Failed PeerConnection would use.
// Recovery deliberately goes through Stop(false) +
// OBS_OUTPUT_DISCONNECTED rather than any bespoke restart logic, so a
// stall reuses the one reconnect path that is already exercised by the
// grace timer and by Failed.
//
// Set whip_watchdog_interval_sec to 0 to disable entirely, or
// whip_watchdog_stall_sec to 0 to keep the heartbeat logging but never
// act on it (useful when diagnosing whether a stall is real).
void WHIPOutput::StartWatchdog(uint64_t generation)
{
	// Reap any previous session's watchdog first, before the disable
	// check below can return early and strand it. Joining here cannot
	// deadlock even though this runs on start_stop_thread and a tripped
	// watchdog calls Stop(), which joins start_stop_thread: Stop() calls
	// StopWatchdog() before it acquires start_stop_mutex, and Start()
	// cannot spawn this StartThread without that same mutex. So a
	// watchdog still inside Stop() and a running StartThread are mutually
	// exclusive, and the thread stays owned (never detached) so it can't
	// outlive us.
	StopWatchdog();

	if (watchdog_interval_sec <= 0) {
		do_log(LOG_INFO, "Watchdog disabled");
		return;
	}

	{
		std::lock_guard<std::mutex> lk(watchdog_mutex);
		watchdog_cancel = false;
	}

	if (watchdog_stall_sec > 0)
		do_log(LOG_INFO, "Watchdog started: polling every %ds, reconnecting after %ds without progress",
		       watchdog_interval_sec, watchdog_stall_sec);
	else
		do_log(LOG_INFO, "Watchdog started: polling every %ds, stall recovery disabled (heartbeat log only)",
		       watchdog_interval_sec);

	watchdog_thread = std::thread([this, generation]() {
		size_t last_bytes = total_bytes_sent.load();
		int64_t last_progress_ns = os_gettime_ns();

		for (;;) {
			std::unique_lock<std::mutex> lk(watchdog_mutex);
			bool cancelled = watchdog_cv.wait_for(lk, std::chrono::seconds(watchdog_interval_sec),
							      [this]() { return watchdog_cancel; });
			lk.unlock();

			if (cancelled)
				return;
			// A newer session has taken over; its own watchdog
			// owns the check from here.
			if (!IsActiveGeneration(generation))
				return;

			const size_t bytes = total_bytes_sent.load();
			const int64_t now_ns = os_gettime_ns();

			if (bytes != last_bytes) {
				last_bytes = bytes;
				last_progress_ns = now_ns;
			}

			const double stalled_sec = (double)(now_ns - last_progress_ns) / 1e9;

			// Only meaningful once data capture has actually
			// begun; before that a flat counter is expected.
			if (!running) {
				do_log(LOG_INFO, "Watchdog: connecting, %zu bytes sent so far", bytes);
				continue;
			}

			if (watchdog_stall_sec > 0 && stalled_sec >= (double)watchdog_stall_sec) {
				do_log(LOG_WARNING,
				       "Watchdog: no bytes sent for %.0fs (threshold %ds) - treating session as dead, tearing down and reconnecting",
				       stalled_sec, watchdog_stall_sec);
				MarkDisconnected();
				PrepareReconnect();
				Stop(false);
				obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
				return;
			}

			do_log(LOG_INFO, "Watchdog: alive, %zu bytes sent, %.0fs since last progress", bytes,
			       stalled_sec);
		}
	});
}

void WHIPOutput::StopWatchdog()
{
	{
		std::lock_guard<std::mutex> lk(watchdog_mutex);
		watchdog_cancel = true;
	}
	watchdog_cv.notify_all();

	// Same self-join guard as CancelDisconnectGraceTimer(): the watchdog
	// thread itself calls Stop() -> StopWatchdog() when it trips.
	if (watchdog_thread.joinable() && watchdog_thread.get_id() != std::this_thread::get_id())
		watchdog_thread.join();
}

void WHIPOutput::CancelDisconnectGraceTimer()
{
	{
		std::lock_guard<std::mutex> lk(disconnect_grace_mutex);
		disconnect_grace_cancel = true;
	}
	disconnect_grace_cv.notify_all();

	// Guard against self-join: this is called from Stop(), which the
	// timer thread itself calls once its grace period expires. Joining
	// our own thread would deadlock (or throw), so just leave it to be
	// reaped the next time this is called from a different thread.
	if (disconnect_grace_thread.joinable() && disconnect_grace_thread.get_id() != std::this_thread::get_id())
		disconnect_grace_thread.join();
}

void WHIPOutput::Send(void *data, uintptr_t size, uint64_t duration, std::shared_ptr<rtc::Track> track,
		      std::shared_ptr<rtc::RtcpSrReporter> rtcp_sr_reporter)
{
	if (track == nullptr || !track->isOpen())
		return;

	std::vector<rtc::byte> sample{(rtc::byte *)data, (rtc::byte *)data + size};

	auto rtp_config = rtcp_sr_reporter->rtpConfig;

	// Sample time is in microseconds, we need to convert it to seconds
	auto elapsed_seconds = double(duration) / (1000.0 * 1000.0);

	// Get elapsed time in clock rate
	uint32_t elapsed_timestamp = rtp_config->secondsToTimestamp(elapsed_seconds);

	// Set new timestamp
	rtp_config->timestamp = rtp_config->timestamp + elapsed_timestamp;

#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR < 23
	// Get elapsed time in clock rate from last RTCP sender report
	auto report_elapsed_timestamp = rtp_config->timestamp - rtcp_sr_reporter->lastReportedTimestamp();

	// Check if last report was at least 1 second ago
	if (rtp_config->timestampToSeconds(report_elapsed_timestamp) > 1)
		rtcp_sr_reporter->setNeedsToReport();
#endif

	try {
		track->send(sample);
		total_bytes_sent += sample.size();
	} catch (const std::exception &e) {
		do_log(LOG_ERROR, "error: %s ", e.what());
	}
}

void register_whip_output()
{
	const uint32_t base_flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE | OBS_OUTPUT_MULTI_TRACK_AV;

	const char *audio_codecs = "opus";
#ifdef ENABLE_HEVC
	const char *video_codecs = "h264;hevc;av1";
#else
	const char *video_codecs = "h264;av1";
#endif

	struct obs_output_info info = {};
	info.id = "whip_output";
	info.flags = OBS_OUTPUT_AV | base_flags;
	info.get_name = [](void *) -> const char * {
		return obs_module_text("Output.Name");
	};
	info.create = [](obs_data_t *settings, obs_output_t *output) -> void * {
		return new WHIPOutput(settings, output);
	};
	info.destroy = [](void *priv_data) {
		delete static_cast<WHIPOutput *>(priv_data);
	};
	info.start = [](void *priv_data) -> bool {
		return static_cast<WHIPOutput *>(priv_data)->Start();
	};
	info.stop = [](void *priv_data, uint64_t) {
		static_cast<WHIPOutput *>(priv_data)->Stop();
	};
	info.encoded_packet = [](void *priv_data, struct encoder_packet *packet) {
		static_cast<WHIPOutput *>(priv_data)->Data(packet);
	};
	info.get_defaults = [](obs_data_t *) {
	};
	info.get_properties = [](void *) -> obs_properties_t * {
		return obs_properties_create();
	};
	info.get_total_bytes = [](void *priv_data) -> uint64_t {
		return (uint64_t)static_cast<WHIPOutput *>(priv_data)->GetTotalBytes();
	};
	info.get_connect_time_ms = [](void *priv_data) -> int {
		return static_cast<WHIPOutput *>(priv_data)->GetConnectTime();
	};
	info.encoded_video_codecs = video_codecs;
	info.encoded_audio_codecs = audio_codecs;
	info.protocols = "WHIP";

	obs_register_output(&info);

	info.id = "whip_output_video";
	info.flags = OBS_OUTPUT_VIDEO | base_flags;
	info.encoded_audio_codecs = nullptr;
	obs_register_output(&info);

	info.id = "whip_output_audio";
	info.flags = OBS_OUTPUT_AUDIO | base_flags;
	info.encoded_video_codecs = nullptr;
	info.encoded_audio_codecs = audio_codecs;
	obs_register_output(&info);
}
