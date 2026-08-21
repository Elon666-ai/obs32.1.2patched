#pragma once

#include <string>

// A WHIP publish credential issued by ppcenter for exactly one appId/stream,
// good for a bounded time window (see docs/obs-whip-publish-auth-protocol.md
// on the mmx side). ppobs must fetch a fresh one before every publish
// attempt rather than caching/reusing across reconnects.
struct PPCenterPublishToken {
	std::string whip_url;
	std::string bearer_token;
};

// Calls ppcenter's POST {ppcenter_url}/v1/publish/requests to obtain a
// short-lived WHIP publish bearer token plus the origin node's WHIP URL for
// this ppobs device. This is the ONLY way ppobs authenticates a WHIP
// publish - there is no local fallback secret. Returns false and fills
// `error` with a human-readable message on any failure (network error,
// non-2xx response, malformed response).
bool ppcenter_fetch_publish_token(const std::string &ppcenter_url, const std::string &app_id,
				   const std::string &app_secret, const std::string &stream_name,
				   const std::string &request_region, const std::string &device_uuid,
				   const std::string &user_agent_header, PPCenterPublishToken &out,
				   std::string &error);
