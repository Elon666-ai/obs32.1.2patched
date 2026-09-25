#include "DegradeClient.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>
#include <util/platform.h>

#define do_log(level, fmt, ...) blog(level, "[degrade] " fmt, ##__VA_ARGS__)

// -------------------------------------------------------------------
// URL helpers
// -------------------------------------------------------------------
static bool starts_with(const std::string &s, const char *prefix)
{
	return s.compare(0, strlen(prefix), prefix) == 0;
}

static std::string url_decode(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '%' && i + 2 < in.size()) {
			auto hex = [](char c) -> int {
				if (c >= '0' && c <= '9')
					return c - '0';
				if (c >= 'a' && c <= 'f')
					return c - 'a' + 10;
				if (c >= 'A' && c <= 'F')
					return c - 'A' + 10;
				return -1;
			};
			int hi = hex(in[i + 1]);
			int lo = hex(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back((char)((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(in[i]);
	}
	return out;
}

// Strips userinfo and port from an authority ("user@host:port" -> "host",
// "[::1]:port" -> "[::1]").
static std::string authority_host(const std::string &authority)
{
	std::string host = authority;
	auto at = host.rfind('@');
	if (at != std::string::npos)
		host = host.substr(at + 1);

	if (!host.empty() && host[0] == '[') {
		auto end = host.find(']');
		if (end != std::string::npos)
			host = host.substr(0, end + 1);
	} else {
		auto colon = host.rfind(':');
		if (colon != std::string::npos)
			host = host.substr(0, colon);
	}
	return host;
}

// http(s)://host/<path>/whip -> ws(s)://host/<path>/ws/whip
static std::string whip_to_ws(std::string url)
{
	while (!url.empty() && url.back() == '/')
		url.pop_back();

	if (starts_with(url, "https://"))
		url.replace(0, 8, "wss://");
	else if (starts_with(url, "http://"))
		url.replace(0, 7, "ws://");

	auto p = url.rfind("/whip");
	if (p != std::string::npos) {
		url = url.substr(0, p);
		url += "/ws/whip";
	}
	return url;
}

// srt://host:<port>?streamid=publish:<path>            (custom syntax)
// srt://host:<port>?streamid=#!::m=publish,r=<path>   (standard syntax)
static std::string srt_to_ws(const std::string &url)
{
	std::string rest = url.substr(strlen("srt://"));

	std::string authority;
	std::string query;
	auto q = rest.find('?');
	if (q == std::string::npos) {
		authority = rest;
	} else {
		authority = rest.substr(0, q);
		query = rest.substr(q + 1);
	}

	std::string path;
	auto sid_pos = query.find("streamid=");
	if (sid_pos != std::string::npos) {
		std::string sid = query.substr(sid_pos + strlen("streamid="));
		auto amp = sid.find('&');
		if (amp != std::string::npos)
			sid = sid.substr(0, amp);
		sid = url_decode(sid);

		if (starts_with(sid, "#!::")) {
			auto r = sid.find("r=");
			if (r != std::string::npos) {
				path = sid.substr(r + 2);
				auto comma = path.find(',');
				if (comma != std::string::npos)
					path = path.substr(0, comma);
			}
		} else {
			// "publish:<path>" / "request:<path>"
			auto colon = sid.find(':');
			path = (colon == std::string::npos) ? sid : sid.substr(colon + 1);
		}
	}

	const char *env_port = getenv("WHIP_DEGRADE_WS_PORT");
	std::string port = (env_port && env_port[0]) ? env_port : "8889";

	std::string ws = "ws://" + authority_host(authority) + ":" + port;
	if (!path.empty()) {
		if (path[0] != '/')
			ws += "/";
		ws += path;
	}
	ws += "/ws/whip";
	return ws;
}

std::string DegradeClient::WebSocketUrlForService(obs_service_t *service)
{
	if (const char *override_url = getenv("WHIP_DEGRADE_WS_URL"))
		if (override_url[0])
			return override_url;

	if (!service)
		return {};

	const char *url_c = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (!url_c || !url_c[0])
		return {};

	std::string url(url_c);
	if (starts_with(url, "srt://"))
		return srt_to_ws(url);
	if (starts_with(url, "http://") || starts_with(url, "https://"))
		return whip_to_ws(url);
	return {};
}

// -------------------------------------------------------------------
//  Lifecycle
// -------------------------------------------------------------------
DegradeClient::DegradeClient() : client(), mtx(), running(false), worker() {}

DegradeClient::~DegradeClient()
{
	Stop();
	if (worker.joinable())
		worker.join();
}

void DegradeClient::Start(const std::string &url, TargetCallback cb)
{
	Stop();
	if (url.empty())
		return;

	{
		std::lock_guard<std::mutex> lk(mtx);
		ws_url = url;
		callback = std::move(cb);
		conn.reset();
		reconnect_backoff_ms = kReconnectBackoffMinMs;
		next_reconnect_attempt_ns = 0;
	}

	client = std::make_unique<client_t>();
	client->clear_access_channels(websocketpp::log::alevel::all);
	client->clear_error_channels(websocketpp::log::elevel::all);
	client->init_asio();

	running.store(true);
	worker = std::thread(&DegradeClient::WorkerMain, this);
}

void DegradeClient::Stop()
{
	if (!running.load())
		return;

	running.store(false);

	// io_service::stop() is thread-safe and unblocks a run() that is
	// parked waiting for socket work (an open connection with no traffic
	// keeps run() blocked indefinitely otherwise). The worker then closes
	// the connection itself before exiting, so no websocketpp object is
	// touched from this thread.
	if (client)
		client->stop();
	if (worker.joinable())
		worker.join();
	worker = std::thread();
	client.reset();

	std::lock_guard<std::mutex> lk(mtx);
	callback = nullptr;
	ws_url.clear();
	next_reconnect_attempt_ns = 0;
	conn.reset();
}

void DegradeClient::WorkerMain()
{
	client->set_open_handler([this](handle_t) {
		std::string url;
		{
			std::lock_guard<std::mutex> lk(mtx);
			url = ws_url;
			reconnect_backoff_ms = kReconnectBackoffMinMs;
		}
		do_log(LOG_INFO, "WS connected to %s", url.c_str());
	});

	client->set_close_handler([this](handle_t) {
		std::string reason;
		websocketpp::close::status::value code = websocketpp::close::status::abnormal_close;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (conn) {
				reason = conn->get_remote_close_reason();
				code = conn->get_remote_close_code();
			}
			conn.reset();
			if (!ws_url.empty()) {
				next_reconnect_attempt_ns =
					os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
				reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
			}
		}
		do_log(LOG_INFO, "WS closed (code=%d reason=%s)", (int)code,
		       reason.empty() ? "(none)" : reason.c_str());
	});

	client->set_fail_handler([this](handle_t) {
		std::string ec_msg;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (conn)
				ec_msg = conn->get_ec().message();
			conn.reset();
			if (!ws_url.empty()) {
				next_reconnect_attempt_ns =
					os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
				reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
			}
		}
		do_log(LOG_INFO, "WS fail (ec=%s)", ec_msg.empty() ? "(unknown)" : ec_msg.c_str());
	});

	client->set_message_handler(
		[this](handle_t, client_t::message_ptr msg) { HandleMessage(msg->get_payload()); });

	{
		std::lock_guard<std::mutex> lk(mtx);
		ConnectLocked();
	}

	while (running.load()) {
		if (client->stopped())
			client->reset();
		client->run();
		if (!running.load())
			break;

		os_sleep_ms(50);

		std::lock_guard<std::mutex> lk(mtx);
		if (ShouldReconnectLocked())
			ConnectLocked();
	}

	{
		std::lock_guard<std::mutex> lk(mtx);
		if (conn) {
			websocketpp::lib::error_code ec;
			conn->close(websocketpp::close::status::going_away, "shutdown", ec);
			conn.reset();
		}
	}
	client->stop();
}

// -------------------------------------------------------------------
//  Connection (re)establishment
// -------------------------------------------------------------------
bool DegradeClient::ShouldReconnectLocked() const
{
	return !conn && !ws_url.empty() && next_reconnect_attempt_ns != 0 &&
	       os_gettime_ns() >= next_reconnect_attempt_ns;
}

void DegradeClient::ConnectLocked()
{
	next_reconnect_attempt_ns = 0;

	do_log(LOG_INFO, "Connecting to %s", ws_url.c_str());

	websocketpp::lib::error_code ec;
	conn = client->get_connection(ws_url, ec);
	if (ec) {
		do_log(LOG_ERROR, "Failed to create connection %s: %s", ws_url.c_str(), ec.message().c_str());
		conn.reset();
		next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
		reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
		return;
	}

	// Same static shared secret as the WHIP publish bearer-token fallback
	// (whip-service.cpp): env first, then the dev/test default.
	{
		const char *secret = getenv("WHIP_WS_SECRET");
		if (!secret || !secret[0])
			secret = "de4e53fe0b4565358cf5b47c89cc6dbbc0f902c62e4c2952";

		conn->append_header("Authorization", std::string("Bearer ") + secret);
	}

	client->connect(conn);
}

// -------------------------------------------------------------------
//  Message handling
// -------------------------------------------------------------------
void DegradeClient::HandleMessage(const std::string &payload)
{
	do_log(LOG_DEBUG, "WS Rx: %s", payload.c_str());

	try {
		auto j = nlohmann::json::parse(payload);
		const std::string type = j.value("type", std::string());

		if (type == "TARGET_STATE") {
			int layers = j.contains("layers") ? j.value("layers", 0) : 0;
			int pct = j.contains("bitrate_percent") ? j.value("bitrate_percent", 100) : 100;
			DispatchTarget(layers, pct);
			return;
		}

		if (type == "ALERT") {
			// Protocol §2/§R7: terminate-state notice, informational only.
			std::string path = j.value("path", std::string());
			std::string reason = j.value("reason", std::string());
			do_log(LOG_INFO, "ALERT path=%s reason=%s", path.c_str(), reason.c_str());
			return;
		}
	} catch (const std::exception &e) {
		do_log(LOG_DEBUG, "JSON parse error: %s", e.what());
	}
}

void DegradeClient::DispatchTarget(int layers, int bitrate_percent)
{
	TargetCallback cb;
	{
		std::lock_guard<std::mutex> lk(mtx);
		cb = callback;
	}
	if (cb)
		cb(layers, bitrate_percent);
}
