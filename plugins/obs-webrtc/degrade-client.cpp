#include "degrade-client.h"

#include <algorithm>
#include <cstdlib>
#include <obs.hpp>
#include <nlohmann/json.hpp>
#include <util/platform.h>

#define do_log(level, fmt, ...) blog(level, "[degrade-client] " fmt, ##__VA_ARGS__)

// -------------------------------------------------------------------
// URL helper
// -------------------------------------------------------------------
static std::string whip_to_ws(const std::string &whip_url)
{
	std::string url = whip_url;
	while (!url.empty() && url.back() == '/')
		url.pop_back();

	if (url.compare(0, 8, "https://") == 0)
		url.replace(0, 8, "wss://");
	else if (url.compare(0, 7, "http://") == 0)
		url.replace(0, 7, "ws://");

	auto p = url.find("/whip");
	if (p != std::string::npos) {
		url = url.substr(0, p);
		url += "/ws/whip";
	}
	return url;
}

// -------------------------------------------------------------------
//  Singleton
// -------------------------------------------------------------------
WsDegradeClient &WsDegradeClient::Instance()
{
	static WsDegradeClient instance;
	return instance;
}

WsDegradeClient::WsDegradeClient()
	: client(),
	  conn(),
	  output(nullptr),
	  whip_url(),
	  ws_url(),
	  mtx(),
	  running(true),
	  worker(),
	  last_pct(100)
{
	client.clear_access_channels(websocketpp::log::alevel::all);
	client.clear_error_channels(websocketpp::log::elevel::all);

	client.init_asio();

	client.set_open_handler([this](handle_t) {
		do_log(LOG_INFO, "WS connected to %s", ws_url.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		reconnect_backoff_ms = kReconnectBackoffMinMs;
	});

	client.set_close_handler([this](handle_t) {
		std::string reason;
		websocketpp::close::status::value code = websocketpp::close::status::abnormal_close;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (conn) {
				reason = conn->get_remote_close_reason();
				code = conn->get_remote_close_code();
			}
		}
		do_log(LOG_INFO, "WS closed (%s) code=%d reason=%s",
		       ws_url.c_str(),
		       (int)code,
		       reason.empty() ? "(none)" : reason.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		conn.reset();
		// Schedule a reconnect attempt; the worker loop's idle tick
		// picks this up (see ShouldReconnectLocked/ConnectLocked).
		// Without this, a dropped connection (e.g. code=1006
		// abnormal close) just sits idle forever, silently losing
		// the ability to receive further degrade/recover commands
		// for the rest of the stream.
		next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
		reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
	});

	client.set_fail_handler([this](handle_t) {
		std::string ec_msg;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (conn) {
				ec_msg = conn->get_ec().message();
			}
		}
		do_log(LOG_INFO, "WS fail (%s) ec=%s",
		       ws_url.c_str(),
		       ec_msg.empty() ? "(unknown)" : ec_msg.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		conn.reset();
		next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
		reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
	});

	client.set_message_handler([this](handle_t h, client_t::message_ptr msg) {
		const std::string &payload = msg->get_payload();
		do_log(LOG_DEBUG, "WS Rx: %s", payload.c_str());

		TargetState ts;
		if (ParseTargetState(payload, ts)) {
			ApplyIfNeeded(ts);
			return;
		}

		// Protocol §2: terminate-state ALERT (no action, log only)
		try {
			auto j = nlohmann::json::parse(payload);
			if (j.contains("type") &&
			    j["type"] == "ALERT") {
				std::string path = j.value("path", "");
				std::string reason = j.value("reason", "");
				do_log(LOG_INFO,
				       "ALERT path=%s reason=%s",
				       path.c_str(),
				       reason.c_str());
			}
		} catch (const std::exception &) {
			// not valid JSON for our purpose, ignore
		}
	});

	worker = std::thread([this]() {
		while (running.load()) {
			client.run();
			// brief sleep to avoid busy-loop when no io work
			os_sleep_ms(50);

			std::lock_guard<std::mutex> lk(mtx);
			if (ShouldReconnectLocked())
				ConnectLocked();
		}
	});
}

WsDegradeClient::~WsDegradeClient()
{
	running.store(false);

	{
		std::lock_guard<std::mutex> lk(mtx);
		if (conn) {
			websocketpp::lib::error_code ec;
			conn->close(websocketpp::close::status::going_away, "shutdown", ec);
			conn.reset();
		}
	}

	client.stop();
	if (worker.joinable())
		worker.join();
}

// -------------------------------------------------------------------
//  Connection (re)establishment
// -------------------------------------------------------------------
bool WsDegradeClient::ShouldReconnectLocked() const
{
	return !conn && !ws_url.empty() && next_reconnect_attempt_ns != 0 &&
	       os_gettime_ns() >= next_reconnect_attempt_ns;
}

void WsDegradeClient::ConnectLocked()
{
	next_reconnect_attempt_ns = 0;

	do_log(LOG_INFO, "Connecting to %s", ws_url.c_str());

	websocketpp::lib::error_code ec;
	conn = client.get_connection(ws_url, ec);
	if (ec) {
		do_log(LOG_ERROR, "Failed to create connection %s: %s", ws_url.c_str(), ec.message().c_str());
		conn.reset();
		// Retry later rather than giving up permanently - a
		// transient local resource error shouldn't need a full
		// output restart (RegisterOutput call) to recover from.
		next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
		reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
		return;
	}

	// Read WHIP_WS_SECRET — prefer env var, fall back to the same
	// hardcoded default used by whip-output.cpp's bearer-token fallback
	// (see comment there), so a stock dev/test server with a shared
	// static secret works out of the box.
	{
		const char *secret = getenv("WHIP_WS_SECRET");
		if (!secret || !secret[0])
			secret = "de4e53fe0b4565358cf5b47c89cc6dbbc0f902c62e4c2952";

		conn->append_header("Authorization", std::string("Bearer ") + secret);
		do_log(LOG_INFO, "WS auth: Bearer header set (secret src: %s)",
		       getenv("WHIP_WS_SECRET") ? "env" : "hardcoded-default");
	}

	client.connect(conn);
}

// -------------------------------------------------------------------
//  Output registration
// -------------------------------------------------------------------
void WsDegradeClient::RegisterOutput(obs_output_t *out)
{
	std::lock_guard<std::mutex> lk(mtx);
	output = out;

	do_log(LOG_INFO, "output '%s' registered", obs_output_get_name(out));

	obs_service_t *svc = obs_output_get_service(out);
	if (!svc)
		return;

	const char *url_c = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (!url_c || !url_c[0])
		return;

	std::string new_whip(url_c);
	std::string new_ws = whip_to_ws(new_whip);

	if (new_ws == ws_url)
		return;  // already connected to this endpoint

	whip_url = new_whip;
	ws_url = new_ws;

	// Close old connection
	if (conn) {
		websocketpp::lib::error_code ec;
		conn->close(websocketpp::close::status::going_away, "url-change", ec);
		conn.reset();
	}

	// Fresh endpoint: reset backoff state so the first attempt on it
	// isn't delayed by whatever the previous endpoint's failures ran up.
	reconnect_backoff_ms = kReconnectBackoffMinMs;
	ConnectLocked();
}

void WsDegradeClient::UnregisterOutput()
{
	std::lock_guard<std::mutex> lk(mtx);
	output = nullptr;
	do_log(LOG_INFO, "Output unregistered");
}

// ---------------------------------------------------------------------
//  JSON parsing
// ---------------------------------------------------------------------
bool WsDegradeClient::ParseTargetState(const std::string &json, TargetState &out)
{
	try {
		auto j = nlohmann::json::parse(json);
		if (!j.contains("type") || j["type"] != "TARGET_STATE")
			return false;
		// "layers" (if present) is intentionally ignored - see the
		// comment on TargetState in degrade-client.h.
		if (j.contains("bitrate_percent"))
			out.bitrate_percent = j["bitrate_percent"];
	} catch (const std::exception &e) {
		do_log(LOG_DEBUG, "JSON parse error: %s", e.what());
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------
//  Apply TARGET_STATE
// ---------------------------------------------------------------------
//
// Only ever touches bitrate, via obs_encoder_update() - which, per
// obs-encoder.c, is safe to call on an already-active encoder (the new
// settings are applied on the encoder thread at the next opportunity
// instead of being dropped) - so this never needs to stop/restart the
// output. Simulcast layer count is never modified here; see the comment
// on TargetState in degrade-client.h for why.
void WsDegradeClient::ApplyIfNeeded(const TargetState &target)
{
	std::lock_guard<std::mutex> lk(mtx);

	if (!output) {
		do_log(LOG_DEBUG, "no output registered, skip");
		return;
	}

	do_log(LOG_INFO, "TARGET_STATE bitrate=%d%% | current bitrate=%d%%", target.bitrate_percent, last_pct);

	// Idempotency check
	if (last_pct == target.bitrate_percent) {
		do_log(LOG_DEBUG, "already at target bitrate, skipping");
		return;
	}

	last_pct = target.bitrate_percent;

	for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		auto *enc = obs_output_get_video_encoder2(output, i);
		if (!enc)
			break;

		OBSDataAutoRelease s = obs_encoder_get_settings(enc);
		int b = (int)obs_data_get_int(s, "bitrate");
		if (b < 1)
			b = 20000;

		// Cache base bitrate (per encoder) the first time we scale
		// it, so repeated degrade/recover messages always scale off
		// the original bitrate rather than compounding off a
		// previously-scaled value.
		int base = (int)obs_data_get_int(s, "base_bitrate");
		if (base == 0) {
			obs_data_set_int(s, "base_bitrate", b);
			base = b;
		}

		long long scaled = (long long)base * target.bitrate_percent / 100LL;
		if (scaled < 1)
			scaled = 1;
		obs_data_set_int(s, "bitrate", (int)scaled);

		obs_encoder_update(enc, s);
	}

	do_log(LOG_INFO, "Applied bitrate=%d%% (live update, no restart)", target.bitrate_percent);
}
