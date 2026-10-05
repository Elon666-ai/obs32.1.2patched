#include "player-link.hpp"
#include "srt-auth.hpp"
#include "stream-ladder.hpp"

#include <sstream>
#include <iomanip>
#include <cctype>

std::string PlayerLink::URLEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
            escaped << std::nouppercase;
        }
    }
    return escaped.str();
}

std::string PlayerLink::BuildURL(const std::string& baseURL,
                                  const std::string& siteName,
                                  const std::map<std::string, std::string>& streamNames,
                                  const std::map<std::string, std::string>& extraParams) {
    std::ostringstream url;
    url << baseURL;

    // If base URL already has query params, append with &; else start with ?
    bool hasQuery = (baseURL.find('?') != std::string::npos);
    auto addParam = [&](const std::string& key, const std::string& value) {
        if (value.empty()) return;
        url << (hasQuery ? "&" : "?");
        hasQuery = true;
        url << URLEncode(key) << "=" << URLEncode(value);
    };

    // Add site and view
    if (!siteName.empty()) {
        std::string resolvedSite = StreamLadder::SiteNameForStream(siteName);
        std::string viewName = StreamLadder::ViewNameForStream(siteName);
        addParam("site", resolvedSite);
        if (!viewName.empty())
            addParam("view", viewName);
    }

    // Add stream names by level
    static const char* levels[] = {"bottom", "economic", "standard", "standard_hevc", "high"};
    for (auto* level : levels) {
        auto it = streamNames.find(level);
        if (it != streamNames.end() && !it->second.empty()) {
            std::string key = (std::string(level) == "standard_hevc") ? "standardHevc" : level;
            addParam(key, it->second);
        }
    }

    // Add extra parameters
    for (auto& [key, value] : extraParams) {
        addParam(key, value);
    }

    return url.str();
}

std::string PlayerLink::FromConfig(const std::string& baseURL,
                                    const MultipusherConfig& cfg) {
    std::map<std::string, std::string> names;
    for (auto& s : cfg.streams) {
        names[s.level] = SrtAuth::ResolveStreamName(cfg.siteName, s.streamName);
    }
    return BuildURL(baseURL, cfg.siteName, names);
}

std::string PlayerLink::EmbeddedURL(const std::string& baseURL,
                                     const MultipusherConfig& cfg) {
    return FromConfig(baseURL, cfg) + "&embedded=1&autoplay=1&preview=grid&quality=auto&muted=1";
}
