#pragma once

#include "motion-roi.h"
#include "quality-score.h"

#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/base.h>
#include <util/dstr.h>

#include <string>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <algorithm>

#include <rtc/rtc.hpp>

struct videoLayerState {
	uint16_t sequenceNumber;
	uint32_t rtpTimestamp;
	int64_t lastVideoTimestamp;
	uint32_t ssrc;
	std::string rid;
};

class WHIPOutput {
public:
	WHIPOutput(obs_data_t *settings, obs_output_t *output);
	~WHIPOutput();

	bool Start();
	void Stop(bool signal = true);
	void Data(struct encoder_packet *packet);

	inline size_t GetTotalBytes() { return total_bytes_sent; }

	inline int GetConnectTime() { return connect_time_ms; }

private:
	void ConfigureAudioTrack(std::string media_stream_id, std::string cname);
	void ConfigureVideoTrack(std::string media_stream_id, std::string cname);
	bool Init();
	bool Setup(uint64_t generation);
	bool Connect(uint64_t generation, std::string &resourceURL);
	void StartThread(uint64_t generation);
	void SendDelete(const std::string &resourceURL, uint64_t generation, const char *reason);
	void StopThread(bool signal, uint64_t generation, std::string resourceURL);
	void ParseLinkHeader(std::string linkHeader, std::vector<rtc::IceServer> &iceServers);
	void Send(void *data, uintptr_t size, uint64_t duration, std::shared_ptr<rtc::Track> track,
		  std::shared_ptr<rtc::RtcpSrReporter> rtcp_sr_reporter);
	bool IsActiveGeneration(uint64_t generation) const;
	void MarkDisconnected();
	void MarkConnected();
	void PrepareReconnect();
	bool ShouldFallback(const std::string &backup_url);
	void StartDisconnectGraceTimer(uint64_t generation);
	void CancelDisconnectGraceTimer();
	void StartWatchdog(uint64_t generation);
	void StopWatchdog();
	void ApplyRoi();

	obs_output_t *output;

	std::string endpoint_url;
	std::string bearer_token;
	std::string resource_url;
	std::atomic<uint64_t> active_generation;

	std::atomic<bool> running;

	std::mutex start_stop_mutex;
	std::mutex resource_mutex;
	std::thread start_stop_thread;

	// Guards peer_connection/audio_track/video_track/timestamp_channel
	// (and the SR reporters, which are only ever read/written alongside
	// the track they belong to). Data() runs on an OBS encoder thread
	// and reads these; StartThread()/StopThread() run on start_stop_thread
	// and null them out on teardown. Without this lock, Data() can read a
	// shared_ptr while it's concurrently being reset elsewhere - a data
	// race on the shared_ptr control block itself, which corrupts memory.
	// Root-caused a hung/crashed OBS process after ~11h of overnight
	// streaming (RTC worker thread SEH exception, then eventual hang).
	//
	// A shared_mutex (not a plain mutex) because holding a lock only long
	// enough to snapshot the shared_ptrs is not sufficient by itself:
	// track->send()/peer_connection->close() must not run concurrently,
	// since close() tears down the transport the RTC worker thread uses
	// to actually deliver an in-flight send() - a second, deeper race
	// than the shared_ptr one above, previously seen crashing the RTC
	// worker thread inside datachannel.dll. Data()/Send() hold the shared
	// (reader) lock for the full duration of the send call (multiple
	// encoder threads may send concurrently); StartThread()/StopThread()
	// take the exclusive (writer) lock around peer_connection->close()
	// and nulling the members, so a close() can never run while a send()
	// is still in flight.
	std::shared_mutex tracks_mutex;

	uint32_t base_ssrc;
	std::shared_ptr<rtc::PeerConnection> peer_connection;
	std::shared_ptr<rtc::Track> audio_track;
	std::shared_ptr<rtc::Track> video_track;
	std::shared_ptr<rtc::RtcpSrReporter> audio_sr_reporter;
	std::shared_ptr<rtc::RtcpSrReporter> video_sr_reporter;

	// Data channel carrying {frame_no, timestamp, rid} JSON per video
	// frame per simulcast layer; see docs/obs-abs-timestamp-protocol.md.
	std::shared_ptr<rtc::DataChannel> timestamp_channel;

	std::map<obs_encoder_t *, std::shared_ptr<videoLayerState>> videoLayerStates;

	// Motion-driven dynamic ROI (balls + people); started/stopped from
	// ApplyRoi() based on the service's "detect_roi" setting.
	MotionRoiDetector motion_roi;

	// Full-reference quality score (program feed = 100) for the top
	// layer; started from Start() based on the service's "quality_score"
	// setting, fed from Data().
	QualityScorer quality_scorer;

	std::atomic<size_t> total_bytes_sent;
	std::atomic<int> connect_time_ms;
	int64_t start_time_ns;
	int64_t last_audio_timestamp;

	// Fallback / failover state. Written from both the start/stop thread
	// (Init/StartThread) and the PeerConnection state-change callback
	// (which runs on a different thread), so these need to be atomic
	// for cross-thread visibility even though they're simple flags.
	std::atomic<bool> using_backup{false};
	std::atomic<int64_t> fail_since_ns{0};
	std::atomic<bool> fail_since_set{false};
	std::atomic<int> reconnect_attempt{0};
	int disconnect_grace_sec = 10;
	int reconnect_backoff_sec = 3;

	// Grace period before a PeerConnection "Disconnected" state is
	// treated as a real failure (see docs on WHIP_DISCONNECT_GRACE_SEC
	// in whip-output.cpp for why this exists).
	std::thread disconnect_grace_thread;
	std::mutex disconnect_grace_mutex;
	std::condition_variable disconnect_grace_cv;
	bool disconnect_grace_cancel = false;

	// Periodic liveness check; see StartWatchdog() in whip-output.cpp.
	// Catches a session that is nominally "running" but has stopped
	// actually pushing bytes, which no PeerConnection state change
	// would report on its own.
	std::thread watchdog_thread;
	std::mutex watchdog_mutex;
	std::condition_variable watchdog_cv;
	bool watchdog_cancel = false;
	int watchdog_interval_sec = 10;
	int watchdog_stall_sec = 30;
};

void register_whip_output();
