#include "PacketLossMonitor.hpp"

#include <obs-module.h>

#include <callback/calldata.h>
#include <util/platform.h>

#include <algorithm>

namespace {

// Below this many attempted packets in the window the ratio is too noisy to
// act on (low-bitrate stretches, encoder stalls, sparse RTCP reports).
constexpr uint64_t kMinExpectedPackets = 500;

// Below this many attempted packets in a single period the interval ratio is
// too noisy to report (one lost packet out of a handful would look like a
// large percentage).
constexpr uint64_t kMinIntervalPackets = 100;

// Any loss rate above this is written to the OBS log, independent of the
// reconnect threshold.
constexpr double kLogLossPercent = 0.5;

bool QueryOutputCounters(obs_output_t *output, uint64_t &expected, uint64_t &lost, const char *&protocol)
{
	proc_handler_t *ph = obs_output_get_proc_handler(output);
	if (!ph)
		return false;

	// Whichever proc the output registered wins; an output is only ever
	// one of the two protocols.
	static const struct {
		const char *proc;
		const char *label;
	} sources[] = {
		{"get_srt_stats", "SRT"},
		{"get_rtp_stats", "RTP"},
	};

	for (const auto &source : sources) {
		calldata_t cd;
		calldata_init(&cd);

		if (!proc_handler_call(ph, source.proc, &cd)) {
			calldata_free(&cd);
			continue;
		}

		const bool valid = calldata_bool(&cd, "valid");
		if (valid) {
			expected = (uint64_t)std::max<long long>(0, calldata_int(&cd, "packets_expected"));
			lost = (uint64_t)std::max<long long>(0, calldata_int(&cd, "packets_lost"));
			protocol = source.label;
			calldata_free(&cd);
			return true;
		}

		calldata_free(&cd);
	}

	return false;
}

} // namespace

void PacketLossMonitor::Reset()
{
	next_sample_ns = 0;
	have_last = false;
	last_expected = 0;
	last_lost = 0;
	window.clear();
}

bool PacketLossMonitor::QueryCounters(obs_output_t *output, uint64_t &expected, uint64_t &lost, const char *&protocol) const
{
	return QueryOutputCounters(output, expected, lost, protocol);
}

void PacketLossMonitor::Tick(obs_output_t *output, const PacketLossReconnectConfig &cfg)
{
	if (!output || !cfg.enabled) {
		Reset();
		return;
	}

	const int period = cfg.period_sec > 0 ? cfg.period_sec : 10;
	const int window_sec = cfg.window_sec > 0 ? cfg.window_sec : 60;
	const double threshold = cfg.threshold_percent > 0.0 ? cfg.threshold_percent : 1.2;

	const uint64_t now = os_gettime_ns();
	const uint64_t period_ns = (uint64_t)period * 1000000000ULL;

	if (next_sample_ns == 0)
		next_sample_ns = now + period_ns;
	if (now < next_sample_ns)
		return;

	// Advance relative to the scheduled time to avoid drift, but never
	// stack up missed samples after a long stall.
	next_sample_ns += period_ns;
	if (next_sample_ns < now)
		next_sample_ns = now + period_ns;

	uint64_t expected = 0;
	uint64_t lost = 0;
	const char *protocol = "?";
	if (!QueryCounters(output, expected, lost, protocol))
		return;

	if (!have_last) {
		have_last = true;
		last_expected = expected;
		last_lost = lost;
		return;
	}

	// Counters are cumulative for one session; a drop means the output
	// reconnected and we must re-baseline instead of booking a huge
	// negative delta.
	if (expected < last_expected || lost < last_lost) {
		last_expected = expected;
		last_lost = lost;
		window.clear();
		return;
	}

	const uint64_t delta_expected = expected - last_expected;
	const uint64_t delta_lost = lost - last_lost;
	last_expected = expected;
	last_lost = lost;

	if (delta_expected == 0)
		return;

	const double interval_percent = (double)delta_lost / (double)delta_expected * 100.0;

	const size_t max_samples = (size_t)std::max(1, (window_sec + period - 1) / period);
	window.push_back({delta_expected, delta_lost});
	while (window.size() > max_samples)
		window.pop_front();

	const bool have_window = window.size() >= max_samples;
	uint64_t sum_expected = 0;
	uint64_t sum_lost = 0;
	if (have_window) {
		for (const Sample &sample : window) {
			sum_expected += sample.expected;
			sum_lost += sample.lost;
		}
	}
	const double window_percent = sum_expected > 0 ? (double)sum_lost / (double)sum_expected * 100.0 : 0.0;

	// Record any meaningful loss to the log, regardless of the reconnect
	// threshold. The windowed figure is the one the reconnect decision
	// uses; the interval figure catches a spike before the window fills.
	const bool log_interval = delta_expected >= kMinIntervalPackets && interval_percent > kLogLossPercent;
	const bool log_window = have_window && sum_expected >= kMinExpectedPackets && window_percent > kLogLossPercent;

	if (log_window || log_interval) {
		if (log_window)
			blog(LOG_WARNING, "%s packet loss: %.2f%% over the last %ds, %.2f%% in the last %ds",
			     protocol, window_percent, window_sec, interval_percent, period);
		else
			blog(LOG_WARNING, "%s packet loss: %.2f%% in the last %ds", protocol, interval_percent,
			     period);
	}

	if (!have_window || sum_expected < kMinExpectedPackets)
		return;

	if (window_percent <= threshold)
		return;

	blog(LOG_WARNING,
	     "Packet loss %.2f%% over the last %ds exceeds %.2f%% (lost %llu of %llu packets) - "
	     "disconnecting to reconnect",
	     window_percent, window_sec, threshold, (unsigned long long)sum_lost,
	     (unsigned long long)sum_expected);

	// obs_output_signal_stop(OBS_OUTPUT_DISCONNECTED) only reconnects when
	// the output has reconnect retries configured. The feature exists to
	// force a reconnect, so make sure at least the configured retries are
	// available even if the generic Reconnect option was turned off.
	if (!cfg.reconnect_enabled || cfg.max_retries <= 0) {
		const int retries = cfg.max_retries > 0 ? cfg.max_retries : 25;
		const int delay = cfg.is_whip ? 0 : (cfg.retry_delay_sec > 0 ? cfg.retry_delay_sec : 2);
		obs_output_set_reconnect_settings(output, retries, delay);
		blog(LOG_WARNING,
		     "Packet-loss reconnect: automatic reconnect was disabled, enabling %d retr%s for this output",
		     retries, retries == 1 ? "y" : "ies");
	}

	obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);

	// Fresh window so a slow reconnect does not immediately retrigger.
	Reset();
}
