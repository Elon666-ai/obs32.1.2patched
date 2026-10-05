#pragma once

#include <obs-module.h>

#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <mutex>

/// General utility functions.
namespace Utils {

// ── Plugin file logger ────────────────────────────────────────────

/// Initialize the plugin's own file logger.
/// Must be called once during obs_module_load().
/// Log file is created at <exe_dir>/logs/obs-multipusher-YYYYMMDD.log
void InitPluginLogger();

/// Write a formatted message to the plugin log file (thread-safe).
void PluginFileLog(const char* fmt, ...);

/// Shutdown the plugin file logger. Call during obs_module_unload().
void ShutdownPluginLogger();

/// Get the log directory path (for display / debug).
std::string GetPluginLogDir();

// ── MP_LOG macro ──────────────────────────────────────────────────

/// Logs to BOTH OBS global log AND the plugin's own file log.
/// Usage: MP_LOG(LOG_INFO, "[obs-multipusher] message %s", arg);
#define MP_LOG(level, fmt, ...)                            \
    do {                                                   \
        blog(level, fmt, ##__VA_ARGS__);                   \
        Utils::PluginFileLog(fmt, ##__VA_ARGS__);          \
    } while (0)

// ── Utility functions ─────────────────────────────────────────────

/// Trim leading and trailing whitespace.
inline std::string Trim(const std::string& s) {
    if (s.empty()) return s;
    size_t start = 0, end = s.size();
    while (start < end && isspace(static_cast<unsigned char>(s[start]))) ++start;
    while (end > start && isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

/// Convert string to lowercase.
inline std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return r;
}

/// Check if string starts with prefix (case-insensitive).
inline bool StartsWithCI(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return ToLower(s.substr(0, prefix.size())) == ToLower(prefix);
}

/// Check if string ends with suffix (case-insensitive).
inline bool EndsWithCI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return ToLower(s.substr(s.size() - suffix.size())) == ToLower(suffix);
}

/// Mask txSecret in SRT URL for logging.
inline std::string MaskTxSecret(const std::string& url) {
    auto pos = url.find("txSecret=");
    if (pos == std::string::npos) return url;
    auto start = pos + 9; // len("txSecret=")
    auto end = url.find_first_of(",&", start);
    if (end == std::string::npos) end = url.size();
    auto secret = url.substr(start, end - start);
    if (secret.size() <= 10)
        return url.substr(0, start) + "***" + url.substr(end);
    return url.substr(0, start) + secret.substr(0, 6) + "***" +
           secret.substr(secret.size() - 4) + url.substr(end);
}

/// Stream name from SRT URL's r= parameter.
inline std::string StreamNameFromSRTURL(const std::string& url) {
    auto pos = url.find("r=");
    if (pos == std::string::npos) return "";
    auto start = pos + 2;
    auto end = url.find_first_of(",&", start);
    std::string value = (end == std::string::npos) ? url.substr(start) : url.substr(start, end - start);
    auto slash = value.rfind('/');
    if (slash != std::string::npos)
        return value.substr(slash + 1);
    return value;
}

/// Format a timestamp as HH:MM:SS for log display.
inline std::string FormatTime() {
    auto now = time(nullptr);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return std::string(buf);
}

/// Simple string replacement (all occurrences).
inline std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

} // namespace Utils
