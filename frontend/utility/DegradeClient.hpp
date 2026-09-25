#pragma once

#include <obs.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define ASIO_STANDALONE 1
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

// OBS-side executor for the mmx degrade protocol (see
// docs/obs-mmx-degrade-protocol.md / obs-mmx-degrade-requirements.md).
//
// mmx pushes a declarative TARGET_STATE ({layers, bitrate_percent}) on a
// WebSocket derived from the publish endpoint:
//   WHIP  http(s)://host/<path>/whip       -> ws(s)://host/<path>/ws/whip
//   SRT   srt://host:<port>?streamid=...   -> ws://host:8889/<path>/ws/whip
// (the WebRTC HTTP origin, :8889 by default - override with
// WHIP_DEGRADE_WS_PORT, or the whole URL with WHIP_DEGRADE_WS_URL).
//
// This class only owns the socket, the reconnect/backoff loop and JSON
// parsing. What to *do* with a target (live bitrate change, restart with a
// reduced layer count) is decided by the callback the owner supplies - see
// BasicOutputHandler::ApplyDegradeTarget. It replaces the old
// plugin-side (obs-webrtc) executor, which could not reach the SRT output.
class DegradeClient {
	using client_t = websocketpp::client<websocketpp::config::asio_client>;
	using handle_t = websocketpp::connection_hdl;
	using conn_ptr = client_t::connection_ptr;

public:
	// layers is the target simulcast layer count, bitrate_percent the
	// target percentage of the configured encoder bitrate. Called from
	// the worker thread; must not block indefinitely.
	using TargetCallback = std::function<void(int layers, int bitrate_percent)>;

	DegradeClient();
	~DegradeClient();

	DegradeClient(const DegradeClient &) = delete;
	DegradeClient &operator=(const DegradeClient &) = delete;

	// Starts (or re-targets) the client at ws_url. Assumes any previous
	// session was stopped first; ownership of cb is taken. No-op if
	// already connected to the same endpoint.
	void Start(const std::string &ws_url, TargetCallback cb);
	void Stop();

	// Derives the degrade WS URL for service (WHIP or SRT). Empty if the
	// service is neither, or the endpoint can't be parsed.
	static std::string WebSocketUrlForService(obs_service_t *service);

private:
	void WorkerMain();
	void ConnectLocked();
	bool ShouldReconnectLocked() const;
	void HandleMessage(const std::string &payload);
	void DispatchTarget(int layers, int bitrate_percent);

	std::unique_ptr<client_t> client;
	conn_ptr conn;

	std::string ws_url;
	TargetCallback callback;

	mutable std::mutex mtx;

	std::atomic<bool> running;
	std::thread worker;

	// Reconnect backoff state (protected by mtx). The close/fail handlers
	// reset conn and schedule the next retry; the worker loop's idle tick
	// picks it up once the deadline passes.
	uint64_t next_reconnect_attempt_ns = 0;
	int reconnect_backoff_ms = 2000;
	static constexpr int kReconnectBackoffMinMs = 2000;
	static constexpr int kReconnectBackoffMaxMs = 30000;
};
