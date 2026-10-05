#pragma once

#include <string>
#include <cstdint>

/// Tencent Cloud SRT authentication utilities.
/// Ported from multipusher/utils/tencent.go
namespace SrtAuth {

/// Tencent main publish key (same as Go TX_KEY_MAIN).
constexpr const char* TX_KEY_MAIN = "2d3c5856d013241d1ffb949e98e7c6d1";

/// Tencent backup publish key (same as Go TX_KEY_BACK).
constexpr const char* TX_KEY_BACK = "YNG7cTscaxDXXh3hJjc2";

/// Default token validity in days.
constexpr int TX_PUBLISH_TOKEN_DAYS = 90;

/// Generate txTime (hex unix timestamp) and txSecret (MD5 lowercase hex)
/// for a given stream key and token lifetime in days.
///
/// @param streamKey  The stream name (e.g. "3drush-fwh_standard")
/// @param tokenDays  Number of days the token is valid (default 30)
/// @param[out] txTime   Hex upper-case unix timestamp at expiry
/// @param[out] txSecret Lower-case MD5 hex string
void GenTxSecret(const std::string& streamKey, int tokenDays,
                 std::string& txTime, std::string& txSecret);

/// Compute MD5 hash and return lowercase hex string.
std::string MD5Hex(const std::string& input);

/// Build a complete Tencent SRT publish URL.
///
/// Result format:
///   srt://{host}:{port}?streamid=#!::h={host},r={app}/{streamKey},txSecret={secret},txTime={txTime}
std::string BuildSRTURL(const std::string& host, int port,
                        const std::string& app, const std::string& streamKey,
                        int tokenDays = TX_PUBLISH_TOKEN_DAYS);

/// Extract the stream key from an SRT URL "r=" parameter value.
std::string ExtractStreamKey(const std::string& rValue);

/// Resolve a stream name template (e.g. "{siteName}_economic") with an actual site name.
std::string ResolveStreamName(const std::string& siteName, const std::string& template_);

} // namespace SrtAuth
