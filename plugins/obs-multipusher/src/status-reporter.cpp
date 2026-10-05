#include "status-reporter.hpp"
#include "config-manager.hpp"
#include "stream-ladder.hpp"

#include <obs-module.h>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

#include "utils.hpp"

#include <ctime>
#include <sstream>
#include <chrono>

// ── JSON building helpers (lightweight, no external lib) ──────────

static std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string buildStatusJSON(const StreamLadder& ladder,
                                    const std::string& statusOverride) {
    auto& cfg = ladder.Config();
    auto statuses = ladder.GetStatuses();

    // UTC timestamp in RFC3339
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", &utc);

    std::ostringstream json;
    json << "{\n";
    json << "  \"site\": \"" << escapeJson(StreamLadder::SiteNameForStream(cfg.siteName)) << "\",\n";
    json << "  \"view\": \"" << escapeJson(StreamLadder::ViewNameForStream(cfg.siteName)) << "\",\n";
    json << "  \"source\": \"obs-multipusher\",\n";
    json << "  \"version\": \"" << StatusReporter::VERSION << "\",\n";
    json << "  \"reportedAt\": \"" << timeBuf << "\",\n";
    json << "  \"streams\": [\n";

    bool first = true;
    for (auto& os : statuses) {
        if (os.audioOnly) continue; // skip audio-only in status report
        if (!first) json << ",\n";
        first = false;

        std::string status = statusOverride.empty() ? os.status : statusOverride;

        json << "    {\n";
        json << "      \"level\": \"" << escapeJson(os.level) << "\",\n";
        json << "      \"streamName\": \"" << escapeJson(os.streamName) << "\",\n";
        json << "      \"status\": \"" << escapeJson(status) << "\",\n";
        json << "      \"codec\": \"" << escapeJson(os.codec) << "\",\n";
        json << "      \"resolution\": \"" << escapeJson(os.resolution) << "\",\n";
        json << "      \"bitrate\": \"" << escapeJson(os.bitrate) << "\",\n";
        json << "      \"updatedAt\": \"" << timeBuf << "\",\n";
        json << "      \"source\": \"obs-multipusher\",\n";
        json << "      \"version\": \"" << StatusReporter::VERSION << "\"\n";
        json << "    }";
    }
    json << "\n  ]\n";
    json << "}";
    return json.str();
}

// ── HTTP POST implementation ─────────────────────────────────────

#ifdef _WIN32

static bool httpPost(const std::string& url, const std::string& body) {
    // Parse URL
    std::string host, path;
    bool useSSL = false;
    int port = 80;

    if (url.compare(0, 8, "https://") == 0) {
        useSSL = true;
        port = 443;
        auto rest = url.substr(8);
        auto slash = rest.find('/');
        if (slash != std::string::npos) {
            host = rest.substr(0, slash);
            path = rest.substr(slash);
        } else {
            host = rest;
            path = "/";
        }
    } else if (url.compare(0, 7, "http://") == 0) {
        auto rest = url.substr(7);
        auto slash = rest.find('/');
        if (slash != std::string::npos) {
            host = rest.substr(0, slash);
            path = rest.substr(slash);
        } else {
            host = rest;
            path = "/";
        }
    } else {
        host = url;
        path = "/";
    }

    // Check for custom port
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::stoi(host.substr(colon + 1));
        host = host.substr(0, colon);
    }

    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSession = WinHttpOpen(L"obs-multipusher/1.1",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(),
                                         static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = useSSL ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Set headers
    LPCWSTR headers = L"Content-Type: application/json; charset=utf-8\r\n";
    bool ok = WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1),
                                  const_cast<char*>(body.data()),
                                  static_cast<DWORD>(body.size()),
                                  static_cast<DWORD>(body.size()), 0) == TRUE;

    if (ok) {
        ok = WinHttpReceiveResponse(hRequest, nullptr) == TRUE;
        if (ok) {
            DWORD statusCode = 0;
            DWORD size = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
                                WINHTTP_NO_HEADER_INDEX);
            ok = (statusCode >= 200 && statusCode < 300);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

#else // Linux/macOS — use libcurl

static bool httpPost(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && httpCode >= 200 && httpCode < 300;
}
#endif

// ── StatusReporter ────────────────────────────────────────────────

void StatusReporter::SendNow(const StreamLadder& ladder, const std::string& statusOverride) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mu_);
        url = backendURL_;
    }
    if (url.empty()) return;

    std::string json = buildStatusJSON(ladder, statusOverride);
    DoPost(json);
}

void StatusReporter::SendFinalStatus(const StreamLadder& ladder,
                                      const std::string& statusOverride) {
    SendNow(ladder, statusOverride.empty() ? "Stopped" : statusOverride);
}

void StatusReporter::SetBackendURL(const std::string& url) {
    std::lock_guard<std::mutex> lock(mu_);
    backendURL_ = url;
    // Also read from environment on first set
    if (backendURL_.empty()) {
        const char* env = getenv("ABRPLAYER_BACKEND_URL");
        if (env) backendURL_ = env;
    }
}

std::string StatusReporter::BackendURL() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!backendURL_.empty()) return backendURL_;
    // Fallback to environment
    const char* env = getenv("ABRPLAYER_BACKEND_URL");
    return env ? env : "";
}

void StatusReporter::DoPost(const std::string& json) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mu_);
        url = backendURL_;
    }
    if (url.empty()) return;

    // Append /api/pusher/status path
    if (url.back() != '/') url += '/';
    url += "api/pusher/status";

    bool ok = httpPost(url, json);
    if (!ok) {
        std::string error = "HTTP POST failed";
        if (lastError_ != error) {
            MP_LOG(LOG_WARNING, "[obs-multipusher] status report failed: %s", error.c_str());
            lastError_ = error;
        }
    } else {
        if (!lastError_.empty()) {
            MP_LOG(LOG_INFO, "[obs-multipusher] status report recovered");
            lastError_.clear();
        }
    }
}
