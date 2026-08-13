#pragma once

#include <obs-module.h>

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

#define ASIO_STANDALONE 1
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

struct TargetState {
	std::string path;
	int layers = 3;
	int bitrate_percent = 100;
};

class WsDegradeClient {
	using client_t = websocketpp::client<websocketpp::config::asio_client>;
	using handle_t = websocketpp::connection_hdl;
	using conn_ptr = client_t::connection_ptr;

public:
	static WsDegradeClient &Instance();

	void RegisterOutput(obs_output_t *output);
	void UnregisterOutput();

	TargetState currentTarget() const { return target_; }

private:
	WsDegradeClient();
	~WsDegradeClient();

	WsDegradeClient(const WsDegradeClient &) = delete;
	WsDegradeClient &operator=(const WsDegradeClient &) = delete;

	bool ParseTargetState(const std::string &json, TargetState &out);
	void ApplyIfNeeded(const TargetState &state);

	// Opens a connection to ws_url. Assumes mtx is held and ws_url is
	// non-empty; any existing conn should already be reset by the
	// caller. Shared by RegisterOutput() (first connect / URL change)
	// and the worker loop's reconnect check (same URL, connection
	// dropped - see close/fail handlers and ShouldReconnectLocked).
	void ConnectLocked();

	// True if we should attempt (re)connecting: we have a URL, aren't
	// already connected/connecting, and the backoff delay set by the
	// close/fail handlers has elapsed. Assumes mtx is held.
	bool ShouldReconnectLocked() const;

	client_t client;
	conn_ptr conn;

	obs_output_t *output;
	std::string whip_url;
	std::string ws_url;

	TargetState target_;
	mutable std::mutex mtx;

	std::atomic<bool> running;
	std::thread worker;

	int last_layers;
	int last_pct;

	// Reconnect backoff state (protected by mtx). The close/fail
	// handlers reset conn to null and schedule the next retry via
	// these; the worker loop's idle tick (see the run()/sleep(50) loop)
	// checks ShouldReconnectLocked() and calls ConnectLocked() once the
	// deadline passes. Doubles on each consecutive failure, capped, and
	// resets to the base delay once open_handler fires.
	uint64_t next_reconnect_attempt_ns = 0;
	int reconnect_backoff_ms = 2000;
	static constexpr int kReconnectBackoffMinMs = 2000;
	static constexpr int kReconnectBackoffMaxMs = 30000;

	// Cached encoder pointers (all slots), saved at RegisterOutput time.
	// Index 0 is the highest-resolution (full-res) encoder, index
	// max_layers-1 is the lowest-resolution simulcast layer. We use
	// obs_output_set_video_encoder2(out, nullptr, idx) to temporarily
	// remove the highest-resolution layers when degrading (see
	// docs/obs-mmx-degrade-protocol.md).
	std::vector<obs_encoder_t *> all_encoders;
	int max_layers;
};