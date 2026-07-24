#pragma once

#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/base.h>
#include <util/dstr.h>

#include <string>
#include <atomic>
#include <cstdint>
#include <mutex>
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

	obs_output_t *output;

	std::string endpoint_url;
	std::string bearer_token;
	std::string resource_url;
	std::atomic<uint64_t> active_generation;

	std::atomic<bool> running;

	std::mutex start_stop_mutex;
	std::mutex resource_mutex;
	std::thread start_stop_thread;

	uint32_t base_ssrc;
	std::shared_ptr<rtc::PeerConnection> peer_connection;
	std::shared_ptr<rtc::Track> audio_track;
	std::shared_ptr<rtc::Track> video_track;
	std::shared_ptr<rtc::RtcpSrReporter> audio_sr_reporter;
	std::shared_ptr<rtc::RtcpSrReporter> video_sr_reporter;

	std::map<obs_encoder_t *, std::shared_ptr<videoLayerState>> videoLayerStates;

	std::atomic<size_t> total_bytes_sent;
	std::atomic<int> connect_time_ms;
	int64_t start_time_ns;
	int64_t last_audio_timestamp;
};

void register_whip_output();
