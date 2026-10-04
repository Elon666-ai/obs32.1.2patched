#pragma once

#include <rtc/mediahandler.hpp>
#include <rtc/message.hpp>
#include <rtc/rtp.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// Observes incoming RTCP Sender/Receiver Reports on a send-only WebRTC
// track and accumulates the RTP packet loss the remote endpoint reports
// for our stream. The libdatachannel public API exposes no RTP statistics
// of its own, but incoming RTCP Control messages are still delivered
// through a track's MediaHandler chain, so chaining this after the
// packetizer gives us the receiver's view. The WHIP output's get_rtp_stats
// proc handler exposes the totals to the frontend, which samples them the
// same way it samples SRT's unrecoverable-loss counters.
class RtpLossObserver : public rtc::MediaHandler {
public:
	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;

	uint64_t GetExpected() const { return expected_total.load(std::memory_order_relaxed); }
	uint64_t GetLost() const { return lost_total.load(std::memory_order_relaxed); }

private:
	struct SsrcState {
		bool initialized = false;
		uint32_t last_extended_highest = 0;
		uint32_t last_lost = 0;
	};

	void ParseRtcp(const uint8_t *data, size_t size);
	void Accumulate(const rtc::RtcpReportBlock *block);

	std::mutex mtx;
	std::unordered_map<uint32_t, SsrcState> ssrcs;
	std::atomic<uint64_t> expected_total{0};
	std::atomic<uint64_t> lost_total{0};
};
