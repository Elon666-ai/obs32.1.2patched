#include "plugin-main.hpp"
#include "srt-pusher.hpp"

#ifdef HAS_DOCK_UI
#include "multipusher-dock.hpp"
#endif

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <string>
#include <cstdlib>
#include "utils.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

// ── HTTP GET helper (mirrors httpPost in status-reporter.cpp) ──────

#ifdef _WIN32
static bool httpGet(const std::string& url, std::string& outBody) {
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

    DWORD flags = useSSL ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(),
                                         static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == TRUE;

    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr) == TRUE;

    if (ok) {
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
                            WINHTTP_NO_HEADER_INDEX);
        ok = (statusCode >= 200 && statusCode < 300);

        if (ok) {
            outBody.clear();
            DWORD bytesRead = 0;
            char buf[4096];
            while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
                outBody.append(buf, bytesRead);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}
#else
static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

static bool httpGet(const std::string& url, std::string& outBody) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_easy_cleanup(curl);

    return res == CURLE_OK && httpCode >= 200 && httpCode < 300;
}
#endif

// ── Minimal JSON value extraction (no external lib) ─────────────────

static std::string jsonStringValue(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";

    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";

    return json.substr(pos + 1, end - pos - 1);
}

// ── Push domain fetch ───────────────────────────────────────────────

static std::string buildPushTxURL(const std::string& baseURL) {
    std::string url = baseURL;
    if (!url.empty() && url.back() != '/') url += '/';
    url += "api/push/txUrl";
    return url;
}

static bool fetchPushDomain(const std::string& baseURL, std::string& outHost) {
    if (baseURL.empty()) return false;

    std::string url = buildPushTxURL(baseURL);
    std::string body;
    if (!httpGet(url, body)) return false;

    // Navigate to "data"."host" in the JSON
    outHost = jsonStringValue(body, "host");
    return !outHost.empty();
}

void MultipusherContext::FetchAndApplyPushDomain() {
    const char* envURL = getenv("ABRPLAYER_BACKEND_URL");
    std::string baseURL = envURL ? envURL : "";
    // Fallback to config file if env var not set
    if (baseURL.empty()) {
        baseURL = config.Get().backendURL;
    }

    if (baseURL.empty()) {
        MP_LOG(LOG_INFO, "[obs-multipusher] push domain fetch skipped: backend URL not configured "
               "(set ABRPLAYER_BACKEND_URL env var or backendURL in pusher.local.json)");
        return;
    }

    std::string host;
    if (!fetchPushDomain(baseURL, host)) {
        MP_LOG(LOG_INFO, "[obs-multipusher] push domain fetch failed: cannot reach %s (backend not running or network error)", baseURL.c_str());
        return;
    }

    if (host == config.Get().tencentSrt.host) {
        MP_LOG(LOG_INFO, "[obs-multipusher] push domain unchanged: %s", host.c_str());
        return;
    }

    auto& cfg = config.GetMutable();
    cfg.tencentSrt.host = host;
    config.Save(config.Path());

    MP_LOG(LOG_INFO, "[obs-multipusher] push domain updated from backend: %s", host.c_str());
}

// ── OBS plugin entry points ────────────────────────────────────────
extern "C" {
    OBS_DECLARE_MODULE()
}

// ── Global plugin context ────────────────────────────────────────
std::unique_ptr<MultipusherContext> g_ctx;

// ── MultipusherContext methods ───────────────────────────────────

void MultipusherContext::LoadDefaultConfig() {
    // Primary: absolute path inside the module's data directory so the config
    // persists across OBS restarts regardless of the working directory.
    std::string path;
    char* modFile = obs_module_file("pusher.local.json");
    if (modFile) {
        path = modFile;
        bfree(modFile);
    }

    // Fallback: CWD-relative search for dev environment / first-time migration.
    if (path.empty()) {
        path = ConfigManager::DefaultPath();
    } else {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) {
            // Absolute path doesn't exist yet — check CWD-relative dev paths.
            std::string devPath = ConfigManager::DefaultPath();
            FILE* df = fopen(devPath.c_str(), "r");
            if (df) {
                fclose(df);
                // Load from dev path but will save to absolute path on next write.
                config.Load(devPath);
                config.SetPath(path);
                MP_LOG(LOG_INFO, "[obs-multipusher] migrated config from %s → %s",
                       devPath.c_str(), path.c_str());
                FetchAndApplyPushDomain();
                ApplyConfig();
                return;
            }
        } else {
            fclose(f);
        }
    }

    config.Load(path);
    MP_LOG(LOG_INFO, "[obs-multipusher] config path: %s", path.c_str());
    FetchAndApplyPushDomain();
    ApplyConfig();
}

void MultipusherContext::ApplyConfig() {
    ladder.LoadFromConfig(config.Get());
    outputs.Configure(config.Get(), ladder);

    // Set backend URL for status reporter (env var or config)
    reporter.SetBackendURL(config.Get().backendURL);

    // Wire status callback from output manager → ladder + reporter
    outputs.SetStatusCallback([this](const std::string& streamName,
                                      const std::string& status) {
        ladder.SetStatus(streamName, status);
        if (reporter.BackendURL().empty()) return;
        reporter.SendNow(ladder);
    });

    MP_LOG(LOG_INFO, "[obs-multipusher] config applied: site=%s, backend=%s, %zu active streams",
         config.Get().siteName.c_str(), reporter.BackendURL().c_str(), ladder.ActiveStreams().size());
}

void MultipusherContext::ReloadLadder() {
    ladder.LoadFromConfig(config.Get());
    outputs.Configure(config.Get(), ladder);
}

bool MultipusherContext::StartPublishing() {
    if (publishing) {
        MP_LOG(LOG_WARNING, "[obs-multipusher] already publishing");
        return false;
    }

    // Validate config
    std::string err = config.Validate();
    if (!err.empty()) {
        MP_LOG(LOG_ERROR, "[obs-multipusher] config invalid: %s", err.c_str());
        return false;
    }

    // Save config before starting
    config.Save(config.Path());

    // Reload ladder with latest config
    ReloadLadder();

    // Set initial statuses
    ladder.SetAllStatus("Starting");

    // Start status reporter
    if (!reporter.BackendURL().empty()) {
        MP_LOG(LOG_INFO, "[obs-multipusher] status reporter ready, backend=%s",
             reporter.BackendURL().c_str());
        reporter.SendNow(ladder, "Starting");
    }

    // Apply quality boost setting before starting
    outputs.SetQualityBoost(qualityBoostPercent);

    // Sync OBS streaming service URL with latest SRT URL
    SyncOBSServiceURL();

    // Start OBS outputs
    bool ok = outputs.Start();
    if (ok) {
        publishing = true;
        MP_LOG(LOG_INFO, "[obs-multipusher] publishing started");
    } else {
        ladder.SetAllStatus("Stopped");
        reporter.SendFinalStatus(ladder, "Stopped");
        MP_LOG(LOG_ERROR, "[obs-multipusher] failed to start publishing");
    }

    return ok;
}

void MultipusherContext::StopPublishing() {
    if (!publishing) return;

    publishing = false;
    outputs.Stop();
    ladder.SetAllStatus("Stopped");

    reporter.SendFinalStatus(ladder, "Stopped");

    MP_LOG(LOG_INFO, "[obs-multipusher] publishing stopped");
}

void MultipusherContext::SyncOBSServiceURL() {
    // ── 1. Get the current service URL ─────────────────────────
    obs_service_t* service = obs_frontend_get_streaming_service();
    if (!service) {
        MP_LOG(LOG_INFO, "[obs-multipusher] OBS service sync: no streaming service yet");
        return;
    }

    obs_data_t* curSettings = obs_service_get_settings(service);
    std::string curUrl;
    if (curSettings) {
        curUrl = obs_data_get_string(curSettings, "server");
        obs_data_release(curSettings);
    }
    if (curUrl.empty()) {
        // No URL configured — use the ladder's high-level SRT URL as default
        curUrl = ladder.GetSRTURL("high");
        if (curUrl.empty()) {
            MP_LOG(LOG_WARNING, "[obs-multipusher] OBS service sync: no URL to work with");
            return;
        }
    }

    // ── 2. Extract stream key from r= parameter ───────────────
    std::string rValue;
    auto rPos = curUrl.find(",r=");
    if (rPos != std::string::npos) {
        rPos += 3; // skip ",r="
        auto end = curUrl.find_first_of(",&", rPos);
        rValue = curUrl.substr(rPos, end == std::string::npos ? std::string::npos : end - rPos);
    }
    if (rValue.empty()) {
        MP_LOG(LOG_WARNING, "[obs-multipusher] OBS service sync: cannot extract r= from URL");
        return;
    }
    std::string streamKey = SrtAuth::ExtractStreamKey(rValue);

    // ── 3. Generate fresh tokens ──────────────────────────────
    int tokenDays = config.Get().tencentSrt.tokenDays;
    std::string txTime, txSecret;
    SrtAuth::GenTxSecret(streamKey, tokenDays, txTime, txSecret);

    // ── 4. Replace txSecret= and txTime= in the URL ───────────
    auto replaceParam = [&](const std::string& key, const std::string& newVal) {
        auto pos = curUrl.find("," + key + "=");
        if (pos == std::string::npos) pos = curUrl.find("&" + key + "=");
        if (pos != std::string::npos) {
            pos += key.length() + 2; // skip ",KEY=" or "&KEY="
            auto end = curUrl.find_first_of(",&", pos);
            curUrl.replace(pos, (end == std::string::npos ? curUrl.length() : end) - pos, newVal);
        }
    };
    replaceParam("txSecret", txSecret);
    replaceParam("txTime", txTime);

    // ── 5. Update OBS service ─────────────────────────────────
    obs_data_t* settings = obs_data_create();
    obs_data_set_string(settings, "server", curUrl.c_str());
    obs_service_update(service, settings);
    obs_data_release(settings);

    obs_frontend_save_streaming_service();

    MP_LOG(LOG_INFO, "[obs-multipusher] OBS service token refreshed: stream=%s txSecret=%s*** txTime=%s",
           streamKey.c_str(), txSecret.substr(0, 6).c_str(), txTime.c_str());
}

// ── OBS module lifecycle ────────────────────────────────────────

OBS_MODULE_USE_DEFAULT_LOCALE("obs-multipusher", "en-US")

bool obs_module_load(void) {
    Utils::InitPluginLogger();
    MP_LOG(LOG_INFO, "[obs-multipusher] v%s loading...", PLUGIN_VERSION);

    // Register our custom SRT pusher output type BEFORE anything else
    RegisterSRTPusherOutput();

    g_ctx = std::make_unique<MultipusherContext>();
    g_ctx->LoadDefaultConfig();

    // Log available output types with flags for diagnostics
    {
        size_t idx = 0;
        const char* otId;
        while (obs_enum_output_types(idx, &otId)) {
            uint32_t flags = obs_get_output_flags(otId);
            MP_LOG(LOG_INFO, "[obs-multipusher] output type: %s flags=0x%x encoded=%d service=%d",
                   otId, flags,
                   (flags & OBS_OUTPUT_ENCODED) ? 1 : 0,
                   (flags & OBS_OUTPUT_SERVICE) ? 1 : 0);
            ++idx;
        }
    }

    MP_LOG(LOG_INFO, "[obs-multipusher] loaded successfully");

#ifdef HAS_DOCK_UI
    // Must be called after g_ctx is initialized
    MultipusherDock::Register();
#endif

    return true;
}

void obs_module_unload(void) {
    MP_LOG(LOG_INFO, "[obs-multipusher] unloading...");

    if (g_ctx) {
        g_ctx->StopPublishing();
    }

    g_ctx.reset();
    MP_LOG(LOG_INFO, "[obs-multipusher] unloaded.");
    Utils::ShutdownPluginLogger();
}

const char* obs_module_name(void) {
    return PLUGIN_NAME;
}

const char* obs_module_description(void) {
    return PLUGIN_DESC;
}

const char* obs_module_author(void) {
    return PLUGIN_AUTHOR;
}
