#pragma once

#include <cstdint>
#include <deque>

struct obs_output;
typedef struct obs_output obs_output_t;

// User-configurable knobs for the packet-loss reconnect feature
// (Settings > Advanced > RTP Packet Loss Reconnect).
struct PacketLossReconnectConfig {
	bool enabled = true;
	int period_sec = 30;
	int window_sec = 300;
	double threshold_percent = 1.5;

	// Used only to guarantee the output can actually reconnect when the
	// loss threshold trips, even if the generic Reconnect option is off.
	bool reconnect_enabled = true;
	int max_retries = 25;
	int retry_delay_sec = 2;
	bool is_whip = false;
};

// Samples an output's packet-loss counters every `period_sec` seconds and,
// once a full `window_sec` window of traffic has accumulated, disconnects
// the output when the aggregate loss rate exceeds `threshold_percent`,
// triggering OBS' normal reconnect path.
//
// The counters come from the output's own proc handlers, so the same logic
// covers both protocols:
//   - SRT:  get_srt_stats - cumulative unrecoverable send loss
//   - WHIP: get_rtp_stats - cumulative receiver-reported RTP loss (RTCP RR)
//
// The monitor keeps per-period deltas of the cumulative counters, so the
// aggregate is the true loss rate over the whole window rather than an
// average of percentages.
class PacketLossMonitor {
public:
	void Reset();
	void Tick(obs_output_t *output, const PacketLossReconnectConfig &cfg);

private:
	struct Sample {
		uint64_t expected;
		uint64_t lost;
	};

	bool QueryCounters(obs_output_t *output, uint64_t &expected, uint64_t &lost, const char *&protocol) const;

	uint64_t next_sample_ns = 0;
	bool have_last = false;
	uint64_t last_expected = 0;
	uint64_t last_lost = 0;
	std::deque<Sample> window;
};
