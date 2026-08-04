#include "degrade-client.h"

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
	  target_(),
	  mtx(),
	  running(true),
	  worker(),
	  last_layers(3),
	  last_pct(100)
{
	client.clear_access_channels(websocketpp::log::alevel::all);
	client.clear_error_channels(websocketpp::log::elevel::all);

	client.init_asio();

	client.set_open_handler([this](handle_t) {
		do_log(LOG_INFO, "WS connected to %s", ws_url.c_str());
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
//  Output registration
// -------------------------------------------------------------------
void WsDegradeClient::RegisterOutput(obs_output_t *out)
{
	std::lock_guard<std::mutex> lk(mtx);
	output = out;

	// Cache all video encoder pointers ONCE, the first time we ever see
	// this output (all_encoders starts empty and is never cleared
	// afterwards - see ApplyIfNeeded). RegisterOutput is called again
	// after every degrade/recover-triggered restart, and by then some
	// slots have deliberately been nulled out (fewer layers); rescanning
	// obs_output_get_video_encoder2 at that point would stop at the
	// first null slot and silently shrink max_layers / drop the cached
	// pointers for the higher layers, making it impossible to ever
	// recover back up to the real ceiling.
	if (all_encoders.empty()) {
		max_layers = 0;
		for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			auto *enc = obs_output_get_video_encoder2(out, i);
			if (!enc) break;
			all_encoders.push_back(enc);
			max_layers++;
		}
		do_log(LOG_INFO, "output '%s' registered (%d encoders)",
		       obs_output_get_name(out), max_layers);
	} else {
		do_log(LOG_DEBUG, "output '%s' re-registered after restart (max_layers=%d, unchanged)",
		       obs_output_get_name(out), max_layers);
	}

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

	// Open new connection
	do_log(LOG_INFO, "Connecting to %s", ws_url.c_str());

	websocketpp::lib::error_code ec;
	conn = client.get_connection(ws_url, ec);
	if (ec) {
		do_log(LOG_ERROR, "Failed to create connection %s: %s", ws_url.c_str(), ec.message().c_str());
		conn.reset();
		return;
	}

	// Read WHIP_WS_SECRET — prefer env var, fall back to hard-coded
	// default (must match mmx's WHIP_DEGRADE_WS_SECRET).
	{
		const char *secret = getenv("WHIP_WS_SECRET");
		if (!secret || !secret[0])
			secret = "de4e53fe0b4565358cf5b47c89cc6dbbc0f902c62e4c2952";

		conn->append_header("Authorization",
				    std::string("Bearer ") + secret);
		do_log(LOG_INFO, "WS auth: Bearer header set (secret src: %s)",
		       getenv("WHIP_WS_SECRET") ? "env" : "hardcoded-default");
	}

	client.connect(conn);
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
		if (j.contains("path"))
			out.path = j["path"];
		if (j.contains("layers"))
			out.layers = j["layers"];
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
// obs_output_set_video_encoder2() refuses to do anything (just logs a
// WARNING) while the output is active - see obs-output.c,
// obs_output_set_video_encoder2(): "tried to set video encoder on output
// ... while the output is still active!". So the encoder-slot
// manipulation MUST happen after the output has actually stopped, not
// before obs_output_stop() is even called. obs_output_stop() is
// asynchronous (WHIPOutput::Stop() spins up StopThread), so we poll
// obs_output_active() rather than guessing a fixed sleep duration.
void WsDegradeClient::ApplyIfNeeded(const TargetState &target)
{
	obs_output_t *out_copy = nullptr;
	int new_layers = 0;
	{
		std::lock_guard<std::mutex> lk(mtx);

		if (!output) {
			do_log(LOG_DEBUG, "no output registered, skip");
			return;
		}

		// Read current layer count
		int cur_layers = 0;
		for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			auto *enc = obs_output_get_video_encoder2(output, i);
			if (!enc)
				break;
			cur_layers++;
		}
		if (cur_layers == 0)
			cur_layers = 1;

		do_log(LOG_INFO,
		       "TARGET_STATE layers=%d bitrate=%d%% | current layers=%d bitrate=%d%%",
		       target.layers,
		       target.bitrate_percent,
		       cur_layers,
		       last_pct);

		// Idempotency check
		if (cur_layers == target.layers && last_pct == target.bitrate_percent) {
			do_log(LOG_DEBUG, "already in target state, skipping restart");
			return;
		}

		new_layers = target.layers;
		if (new_layers > max_layers)
			new_layers = max_layers;
		if (new_layers < 1)
			new_layers = 1;

		last_layers = target.layers;
		last_pct = target.bitrate_percent;

		out_copy = output;
	}

	// --- Stop first (outside the mutex: Stop() re-enters this class via
	//     UnregisterOutput(), which also takes mtx) ---
	do_log(LOG_INFO, "Stopping output to apply layers=%d pct=%d%%",
	       new_layers, target.bitrate_percent);
	obs_output_stop(out_copy);

	// Wait for the output to actually go inactive - encoder-slot changes
	// are silently dropped by libobs otherwise. Capped so a stuck
	// output can't wedge this thread forever.
	const int max_wait_ms = 5000;
	int waited_ms = 0;
	while (obs_output_active(out_copy) && waited_ms < max_wait_ms) {
		os_sleep_ms(20);
		waited_ms += 20;
	}
	if (obs_output_active(out_copy)) {
		do_log(LOG_WARNING,
		       "output did not go inactive within %dms, skipping this TARGET_STATE (will retry on next message)",
		       max_wait_ms);
		return;
	}

	{
		std::lock_guard<std::mutex> lk(mtx);

		// --- Manipulate encoder slots so that WHIPOutput::Start()
		//     only sees |new_layers| encoders ---------------------
		//
		// all_encoders[0] is the full-resolution encoder (highest),
		// all_encoders[max_layers-1] is the lowest-resolution
		// simulcast layer (see WHIPSimulcastEncoders::Create /
		// SetStreamOutput). Per docs/obs-mmx-degrade-protocol.md
		// ("层数减少的方向：从高分辨率层开始砍"), reducing layers must
		// drop the highest-resolution layers first and keep the
		// lowest-resolution ones - i.e. keep the tail of
		// all_encoders, not the head.
		//
		// Remove superfluous encoders (null the slot)
		for (int i = new_layers; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			obs_output_set_video_encoder2(out_copy, nullptr, i);
		}

		// Restore the lowest-resolution |new_layers| cached encoders
		int first_kept = max_layers - new_layers;
		for (int i = 0; i < new_layers && (first_kept + i) < (int)all_encoders.size(); i++) {
			obs_output_set_video_encoder2(out_copy, all_encoders[first_kept + i], i);
		}

		// --- Update bitrate on every active encoder ----------
		for (int i = 0; i < new_layers; i++) {
			auto *enc = obs_output_get_video_encoder2(out_copy, i);
			if (!enc) continue;

			OBSDataAutoRelease s = obs_encoder_get_settings(enc);
			int b = (int)obs_data_get_int(s, "bitrate");
			if (b < 1) b = 20000;

			// Cache base bitrate (per encoder)
			int base = (int)obs_data_get_int(s, "base_bitrate");
			if (base == 0) {
				obs_data_set_int(s, "base_bitrate", b);
				base = b;
			}

			long long scaled = (long long)base * target.bitrate_percent / 100LL;
			if (scaled < 1) scaled = 1;
			obs_data_set_int(s, "bitrate", (int)scaled);
		}
	}

	do_log(LOG_INFO, "Starting output (layers=%d pct=%d%%)",
	       new_layers, target.bitrate_percent);
	obs_output_start(out_copy);
}