#include "config-manager.hpp"
#include "srt-auth.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>

#include "utils.hpp"

// ── Helpers ────────────────────────────────────────────────────────

static std::string trim(const std::string& s) {
    if (s.empty()) return s;
    size_t start = 0;
    size_t end = s.size();
    while (start < end && isspace(static_cast<unsigned char>(s[start]))) ++start;
    while (end > start && isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

static std::string lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return r;
}

static bool isSafeSiteName(const std::string& s) {
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

static bool isPositiveEven(int v) { return v > 0 && v % 2 == 0; }

static bool bitrateValid(const std::string& b) {
    if (b.empty()) return false;
    char last = b.back();
    if (last == 'k' || last == 'K' || last == 'm' || last == 'M') {
        for (size_t i = 0; i < b.size() - 1; ++i)
            if (!isdigit(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }
    for (char c : b)
        if (!isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// ── ConfigManager ──────────────────────────────────────────────────

ConfigManager::ConfigManager() {
    cfg_.streams = DefaultStreams();

    // Default scheduled interval: Monday-Sunday, 07:00 -> next-day 06:00.
    IntervalSlot slot;
    slot.startMinutes = 7 * 60;
    slot.endMinutes = 6 * 60;
    for (bool& day : slot.days)
        day = true;
    cfg_.intervals.push_back(slot);
}

std::string ConfigManager::DefaultPath() {
    // Search in same order as Go version
    const char* candidates[] = {
        "conf/pusher.local.json",
        "bin/conf/pusher.local.json",
        "pusher.local.json",
    };
    for (auto* path : candidates) {
        FILE* f = fopen(path, "r");
        if (f) {
            fclose(f);
            return path;
        }
    }
    return candidates[0]; // default even if doesn't exist yet
}

bool ConfigManager::Load(const std::string& path) {
    path_ = path;

    char* jsonStr = os_quick_read_utf8_file(path.c_str());
    if (!jsonStr) {
        MP_LOG(LOG_WARNING, "[obs-multipusher] config file not found: %s, using defaults", path.c_str());
        // Not a fatal error — use defaults
        cfg_.streams = DefaultStreams();
        return false;
    }

    obs_data_t* data = obs_data_create_from_json(jsonStr);
    bfree(jsonStr);
    if (!data) {
        MP_LOG(LOG_ERROR, "[obs-multipusher] failed to parse config JSON: %s", path.c_str());
        return false;
    }

    // siteName
    cfg_.siteName = obs_data_get_string(data, "siteName");

    // backendURL
    cfg_.backendURL = obs_data_get_string(data, "backendURL");

    // tencentSrt
    obs_data_t* srt = obs_data_get_obj(data, "tencentSrt");
    if (srt) {
        cfg_.tencentSrt.host      = obs_data_get_string(srt, "host");
        cfg_.tencentSrt.port      = static_cast<int>(obs_data_get_int(srt, "port"));
        cfg_.tencentSrt.app       = obs_data_get_string(srt, "app");
        cfg_.tencentSrt.tokenDays = static_cast<int>(obs_data_get_int(srt, "tokenDays"));
        obs_data_release(srt);
    }
    if (cfg_.tencentSrt.host.empty())      cfg_.tencentSrt.host = "publish.numericgame.ph";
    if (cfg_.tencentSrt.port <= 0)         cfg_.tencentSrt.port = 9000;
    if (cfg_.tencentSrt.app.empty())       cfg_.tencentSrt.app = "live";
    if (cfg_.tencentSrt.tokenDays <= 0)    cfg_.tencentSrt.tokenDays = 90;

    // input
    obs_data_t* inp = obs_data_get_obj(data, "input");
    if (inp) {
        cfg_.input.mode        = obs_data_get_string(inp, "mode");
        cfg_.input.videoDevice = obs_data_get_string(inp, "videoDevice");
        cfg_.input.audioDevice = obs_data_get_string(inp, "audioDevice");
        cfg_.input.videoFile   = obs_data_get_string(inp, "videoFile");
        cfg_.input.videoLayout = obs_data_get_string(inp, "videoLayout");
        cfg_.input.inputResolution = obs_data_get_string(inp, "inputResolution");
        obs_data_release(inp);
    }
    if (cfg_.input.mode.empty())              cfg_.input.mode = "device";
    if (cfg_.input.videoLayout.empty())       cfg_.input.videoLayout = "portrait";
    if (lower(cfg_.input.inputResolution) == "1080p")
        cfg_.input.inputResolution = "1080P";
    else
        cfg_.input.inputResolution = "4K";

    // streams
    obs_data_array_t* streamsArr = obs_data_get_array(data, "streams");
    if (streamsArr) {
        cfg_.streams.clear();
        size_t count = obs_data_array_count(streamsArr);
        for (size_t i = 0; i < count; ++i) {
            obs_data_t* s = obs_data_array_item(streamsArr, i);
            StreamConfig sc;
            sc.level           = obs_data_get_string(s, "level");
            sc.streamName      = obs_data_get_string(s, "streamName");
            sc.audioOnly       = obs_data_get_bool(s, "audioOnly");
            sc.videoCodec      = obs_data_get_string(s, "videoCodec");
            sc.videoBitrate    = obs_data_get_string(s, "videoBitrate");
            sc.videoMaxrate    = obs_data_get_string(s, "videoMaxrate");
            sc.portraitWidth   = static_cast<int>(obs_data_get_int(s, "portraitWidth"));
            sc.portraitHeight  = static_cast<int>(obs_data_get_int(s, "portraitHeight"));
            sc.landscapeWidth  = static_cast<int>(obs_data_get_int(s, "landscapeWidth"));
            sc.landscapeHeight = static_cast<int>(obs_data_get_int(s, "landscapeHeight"));
            sc.audioCodec      = obs_data_get_string(s, "audioCodec");
            sc.audioBitrate    = obs_data_get_string(s, "audioBitrate");
            cfg_.streams.push_back(sc);
            obs_data_release(s);
        }
        obs_data_array_release(streamsArr);
    }
    if (cfg_.streams.empty())
        cfg_.streams = DefaultStreams();

    NormalizeStreams();

    // publish
    obs_data_t* pub = obs_data_get_obj(data, "publish");
    if (pub) {
        cfg_.publish.onReady             = obs_data_get_bool(pub, "onReady");
        cfg_.publish.reconnectMinSeconds = static_cast<int>(obs_data_get_int(pub, "reconnectMinSeconds"));
        cfg_.publish.reconnectMaxSeconds = static_cast<int>(obs_data_get_int(pub, "reconnectMaxSeconds"));
        obs_data_release(pub);
    }

    // intervals (scheduled-publishing time slots)
    obs_data_array_t* intervalsArr = obs_data_get_array(data, "intervals");
    if (intervalsArr) {
        cfg_.intervals.clear();
        size_t count = obs_data_array_count(intervalsArr);
        for (size_t i = 0; i < count; ++i) {
            obs_data_t* s = obs_data_array_item(intervalsArr, i);
            IntervalSlot slot;
            slot.startMinutes = static_cast<int>(obs_data_get_int(s, "start"));
            slot.endMinutes   = static_cast<int>(obs_data_get_int(s, "end"));
            slot.days[0] = obs_data_get_bool(s, "mon");
            slot.days[1] = obs_data_get_bool(s, "tue");
            slot.days[2] = obs_data_get_bool(s, "wed");
            slot.days[3] = obs_data_get_bool(s, "thu");
            slot.days[4] = obs_data_get_bool(s, "fri");
            slot.days[5] = obs_data_get_bool(s, "sat");
            slot.days[6] = obs_data_get_bool(s, "sun");
            cfg_.intervals.push_back(slot);
            obs_data_release(s);
        }
        obs_data_array_release(intervalsArr);
    }

    obs_data_release(data);

    // Validate after load
    std::string err = Validate();
    if (!err.empty()) {
        MP_LOG(LOG_WARNING, "[obs-multipusher] config validation: %s", err.c_str());
    }

    MP_LOG(LOG_INFO, "[obs-multipusher] loaded config: %s, %zu streams", path.c_str(), cfg_.streams.size());
    return true;
}

bool ConfigManager::Save(const std::string& path) const {
    obs_data_t* data = obs_data_create();

    obs_data_set_string(data, "siteName", cfg_.siteName.c_str());
    obs_data_set_string(data, "backendURL", cfg_.backendURL.c_str());

    obs_data_t* srt = obs_data_create();
    obs_data_set_string(srt, "host", cfg_.tencentSrt.host.c_str());
    obs_data_set_int(srt, "port", cfg_.tencentSrt.port);
    obs_data_set_string(srt, "app", cfg_.tencentSrt.app.c_str());
    obs_data_set_int(srt, "tokenDays", cfg_.tencentSrt.tokenDays);
    obs_data_set_obj(data, "tencentSrt", srt);
    obs_data_release(srt);

    obs_data_t* inp = obs_data_create();
    obs_data_set_string(inp, "mode", cfg_.input.mode.c_str());
    obs_data_set_string(inp, "videoDevice", cfg_.input.videoDevice.c_str());
    obs_data_set_string(inp, "audioDevice", cfg_.input.audioDevice.c_str());
    obs_data_set_string(inp, "videoFile", cfg_.input.videoFile.c_str());
    obs_data_set_string(inp, "videoLayout", cfg_.input.videoLayout.c_str());
    obs_data_set_string(inp, "inputResolution", cfg_.input.inputResolution.c_str());
    obs_data_set_obj(data, "input", inp);
    obs_data_release(inp);

    obs_data_array_t* streamsArr = obs_data_array_create();
    for (auto& sc : cfg_.streams) {
        obs_data_t* s = obs_data_create();
        obs_data_set_string(s, "level", sc.level.c_str());
        obs_data_set_string(s, "streamName", sc.streamName.c_str());
        obs_data_set_bool(s, "audioOnly", sc.audioOnly);
        obs_data_set_string(s, "videoCodec", sc.videoCodec.c_str());
        obs_data_set_string(s, "videoBitrate", sc.videoBitrate.c_str());
        obs_data_set_string(s, "videoMaxrate", sc.videoMaxrate.c_str());
        obs_data_set_int(s, "portraitWidth", sc.portraitWidth);
        obs_data_set_int(s, "portraitHeight", sc.portraitHeight);
        obs_data_set_int(s, "landscapeWidth", sc.landscapeWidth);
        obs_data_set_int(s, "landscapeHeight", sc.landscapeHeight);
        obs_data_set_string(s, "audioCodec", sc.audioCodec.c_str());
        obs_data_set_string(s, "audioBitrate", sc.audioBitrate.c_str());
        obs_data_array_push_back(streamsArr, s);
        obs_data_release(s);
    }
    obs_data_set_array(data, "streams", streamsArr);
    obs_data_array_release(streamsArr);

    obs_data_t* pub = obs_data_create();
    obs_data_set_bool(pub, "onReady", cfg_.publish.onReady);
    obs_data_set_int(pub, "reconnectMinSeconds", cfg_.publish.reconnectMinSeconds);
    obs_data_set_int(pub, "reconnectMaxSeconds", cfg_.publish.reconnectMaxSeconds);
    obs_data_set_obj(data, "publish", pub);
    obs_data_release(pub);

    obs_data_array_t* intervalsArr = obs_data_array_create();
    for (auto& slot : cfg_.intervals) {
        obs_data_t* s = obs_data_create();
        obs_data_set_int(s, "start", slot.startMinutes);
        obs_data_set_int(s, "end", slot.endMinutes);
        obs_data_set_bool(s, "mon", slot.days[0]);
        obs_data_set_bool(s, "tue", slot.days[1]);
        obs_data_set_bool(s, "wed", slot.days[2]);
        obs_data_set_bool(s, "thu", slot.days[3]);
        obs_data_set_bool(s, "fri", slot.days[4]);
        obs_data_set_bool(s, "sat", slot.days[5]);
        obs_data_set_bool(s, "sun", slot.days[6]);
        obs_data_array_push_back(intervalsArr, s);
        obs_data_release(s);
    }
    obs_data_set_array(data, "intervals", intervalsArr);
    obs_data_array_release(intervalsArr);

    std::string json = obs_data_get_json(data);
    obs_data_release(data);

    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        MP_LOG(LOG_ERROR, "[obs-multipusher] cannot write config: %s", path.c_str());
        return false;
    }
    fwrite(json.c_str(), 1, json.size(), f);
    fclose(f);

    MP_LOG(LOG_INFO, "[obs-multipusher] saved config: %s", path.c_str());
    return true;
}

std::string ConfigManager::Validate() const {
    const auto& c = cfg_;

    if (trim(c.siteName).empty())
        return "siteName is required";
    if (!isSafeSiteName(trim(c.siteName)))
        return "siteName can only contain letters, numbers, '_' and '-'";

    if (trim(c.tencentSrt.host).empty())
        return "tencentSrt.host is required";
    if (c.tencentSrt.port <= 0 || c.tencentSrt.port > 65535)
        return "tencentSrt.port must be 1-65535";
    if (trim(c.tencentSrt.app).empty())
        return "tencentSrt.app is required";
    if (c.tencentSrt.tokenDays <= 0)
        return "tencentSrt.tokenDays must be positive";

    if (c.streams.size() < 3 || c.streams.size() > 4)
        return "streams must contain three or four H.264 levels";

    bool hasOrigin = false;
    bool hasHigh = false;
    bool hasStandard = false;
    bool hasEconomic = false;

    for (auto& s : c.streams) {
        std::string resolvedName = SrtAuth::ResolveStreamName(c.siteName, s.streamName);
        if (resolvedName.empty())
            return "streams." + s.level + ".streamName is required";

        if (!isSafeSiteName(resolvedName))
            return "streams." + s.level + ".streamName resolves to invalid stream name: " + resolvedName;

        if (s.audioOnly)
            return "streams." + s.level + ": audio-only outputs are no longer supported";

        if (s.level == "origin")
            hasOrigin = true;
        else if (s.level == "high")
            hasHigh = true;
        else if (s.level == "standard")
            hasStandard = true;
        else if (s.level == "economic")
            hasEconomic = true;
        else
            return "streams." + s.level + ": unknown level (expected origin/high/standard/economic)";

        if (s.audioCodec != "aac")
            return "streams." + s.level + ".audioCodec must be aac";

        if (!bitrateValid(s.audioBitrate))
            return "streams." + s.level + ".audioBitrate is invalid: " + s.audioBitrate;

        std::string vc = lower(s.videoCodec);
        if (vc != "h264" && vc != "libx264" && vc != "h264_qsv" && vc != "h264_nvenc")
            return "streams." + s.level + ".videoCodec must be H.264 (HEVC is no longer pushed)";

        if (!bitrateValid(s.videoBitrate))
            return "streams." + s.level + ".videoBitrate is invalid: " + s.videoBitrate;
        if (!bitrateValid(s.videoMaxrate))
            return "streams." + s.level + ".videoMaxrate is invalid: " + s.videoMaxrate;

        if (!isPositiveEven(s.portraitWidth) || !isPositiveEven(s.portraitHeight) ||
            !isPositiveEven(s.landscapeWidth) || !isPositiveEven(s.landscapeHeight))
            return "streams." + s.level + " dimensions must be positive even numbers";
    }

    if (!hasOrigin || !hasStandard || !hasEconomic)
        return "streams must include origin, standard and economic levels";
    if (c.streams.size() == 4 && !hasHigh)
        return "four-stream layout requires the high level";

    return ""; // OK
}

static StreamConfig MakeOriginStream() {
    return {
        "origin", "{siteName}", false,
        "h264", "5000k", "7500k", 1440, 2560, 2560, 1440,
        "aac", "128k"
    };
}

static StreamConfig MakeHighStream() {
    return {
        "high", "{siteName}_high", false,
        "h264", "3000k", "4500k", 1080, 1920, 1920, 1080,
        "aac", "128k"
    };
}

static StreamConfig MakeStandardStream() {
    return {
        "standard", "{siteName}_standard", false,
        "h264", "1000k", "1500k", 720, 1280, 1280, 720,
        "aac", "128k"
    };
}

static StreamConfig MakeEconomicStream() {
    return {
        "economic", "{siteName}_economic", false,
        "h264", "400k", "600k", 360, 640, 640, 360,
        "aac", "128k"
    };
}

std::vector<StreamConfig> ConfigManager::DefaultStreams() {
    return {
        MakeOriginStream(),
        MakeHighStream(),
        MakeStandardStream(),
        MakeEconomicStream(),
    };
}

void ConfigManager::NormalizeStreams() {
    StreamConfig origin = MakeOriginStream();
    StreamConfig high = MakeHighStream();
    StreamConfig standard = MakeStandardStream();
    StreamConfig economic = MakeEconomicStream();
    bool haveOrigin = false;

    for (auto& s : cfg_.streams) {
        if (s.level == "origin") {
            origin = s;
            haveOrigin = true;
        } else if (s.level == "high") {
            // The old layout used level "high" for the main {siteName}
            // stream. Treat that as the new origin stream (unless an
            // explicit origin level already exists) and synthesize the new
            // 1080p "_high" level.
            std::string resolved = SrtAuth::ResolveStreamName(cfg_.siteName, s.streamName);
            if (resolved == cfg_.siteName || s.streamName == "{siteName}") {
                if (!haveOrigin) {
                    origin = s;
                    haveOrigin = true;
                }
            } else {
                high = s;
            }
        } else if (s.level == "standard") {
            standard = s;
        } else if (s.level == "economic") {
            economic = s;
        }
        // Legacy "bottom" (audio-only) and "standard_hevc" are dropped.
    }

    auto resetVideo = [](StreamConfig& sc, const char* level, const char* streamName, const char* bitrate,
                         const char* maxrate, int pw, int ph, int lw, int lh) {
        sc.level = level;
        sc.streamName = streamName;
        sc.audioOnly = false;
        sc.videoCodec = "h264";
        sc.videoBitrate = bitrate;
        sc.videoMaxrate = maxrate;
        sc.portraitWidth = pw;
        sc.portraitHeight = ph;
        sc.landscapeWidth = lw;
        sc.landscapeHeight = lh;
        sc.audioCodec = "aac";
        if (sc.audioBitrate.empty())
            sc.audioBitrate = "128k";
    };

    resetVideo(origin, "origin", "{siteName}", "5000k", "7500k", 1440, 2560, 2560, 1440);
    resetVideo(high, "high", "{siteName}_high", "3000k", "4500k", 1080, 1920, 1920, 1080);

    // Keep any existing standard/economic bitrates/dimensions when valid,
    // only fill in defaults for missing values and force H.264.
    standard.level = "standard";
    standard.streamName = "{siteName}_standard";
    standard.audioOnly = false;
    standard.videoCodec = "h264";
    if (standard.videoBitrate.empty()) standard.videoBitrate = "1000k";
    if (standard.videoMaxrate.empty()) standard.videoMaxrate = "1500k";
    if (standard.portraitWidth <= 0 || standard.portraitHeight <= 0 ||
        standard.landscapeWidth <= 0 || standard.landscapeHeight <= 0) {
        standard.portraitWidth = 720;
        standard.portraitHeight = 1280;
        standard.landscapeWidth = 1280;
        standard.landscapeHeight = 720;
    }
    standard.audioCodec = "aac";
    if (standard.audioBitrate.empty()) standard.audioBitrate = "128k";

    economic.level = "economic";
    economic.streamName = "{siteName}_economic";
    economic.audioOnly = false;
    economic.videoCodec = "h264";
    if (economic.videoBitrate.empty()) economic.videoBitrate = "400k";
    if (economic.videoMaxrate.empty()) economic.videoMaxrate = "600k";
    if (economic.portraitWidth <= 0 || economic.portraitHeight <= 0 ||
        economic.landscapeWidth <= 0 || economic.landscapeHeight <= 0) {
        economic.portraitWidth = 360;
        economic.portraitHeight = 640;
        economic.landscapeWidth = 640;
        economic.landscapeHeight = 360;
    }
    economic.audioCodec = "aac";
    if (economic.audioBitrate.empty()) economic.audioBitrate = "128k";

    cfg_.streams = {origin, high, standard, economic};
}

std::vector<StreamConfig> ConfigManager::ActiveStreams() const {
    std::vector<StreamConfig> active;
    for (auto& s : cfg_.streams) {
        if (!s.audioOnly)
            active.push_back(s);
    }
    return active;
}
