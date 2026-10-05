#pragma once

#include <string>
#include <map>
#include "config-manager.hpp"

/// Builds ABR player URLs for preview and live monitoring.
/// Ported from multipusher/player_link.go
class PlayerLink {
public:
    /// Default public player URL.
    static constexpr const char* DEFAULT_PLAYER_URL = "https://abrplayer.numericgame.ph/";

    /// Build a player URL given a base URL, site name, and resolved stream names.
    /// Stream names are keyed by level: "high", "standard", "standard_hevc", "economic", "bottom".
    static std::string BuildURL(const std::string& baseURL,
                                const std::string& siteName,
                                const std::map<std::string, std::string>& streamNames,
                                const std::map<std::string, std::string>& extraParams = {});

    /// Build URL from a ConfigManager instance.
    static std::string FromConfig(const std::string& baseURL, const MultipusherConfig& cfg);

    /// Build embedded (local preview) URL with autoplay and grid mode.
    static std::string EmbeddedURL(const std::string& baseURL, const MultipusherConfig& cfg);

private:
    static std::string URLEncode(const std::string& value);
};
