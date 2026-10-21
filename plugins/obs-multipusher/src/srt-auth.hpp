#pragma once

#include <string>
#include <cstdint>

/// Tencent Cloud SRT authentication utilities.
/// Ported from multipusher/utils/tencent.go
namespace SrtAuth {

/// Default token validity in days.
constexpr int TX_PUBLISH_TOKEN_DAYS = 90;

/// Resolve the Tencent publish key at runtime. Secrets are intentionally NOT
/// compiled into the binary; they come from, in order:
///   1. the TX_KEY_MAIN / TX_KEY_BACK environment variable, then
///   2. a local `.env` file (conf/.env, bin/conf/.env, .env, or the plugin
///      data directory).
/// @param backup  true to read TX_KEY_BACK instead of TX_KEY_MAIN.
/// @return the key, or an empty string if none is configured.
std::string GetPublishKey(bool backup = false);

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
