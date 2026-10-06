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
        obs_data_release(inp);
    }
    if (cfg_.input.mode.empty())              cfg_.input.mode = "device";
    if (cfg_.input.videoLayout.empty())       cfg_.input.videoLayout = "portrait";

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

    if (c.streams.size() != 5)
        return "streams must contain exactly five levels";

    for (auto& s : c.streams) {
        std::string resolvedName = SrtAuth::ResolveStreamName(c.siteName, s.streamName);
        if (resolvedName.empty())
            return "streams." + s.level + ".streamName is required";

        if (!isSafeSiteName(resolvedName))
            return "streams." + s.level + ".streamName resolves to invalid stream name: " + resolvedName;

        if (s.audioCodec != "aac")
            return "streams." + s.level + ".audioCodec must be aac";

        if (!bitrateValid(s.audioBitrate))
            return "streams." + s.level + ".audioBitrate is invalid: " + s.audioBitrate;

        if (s.audioOnly)
            continue;

        std::string vc = lower(s.videoCodec);
        if (vc != "h264" && vc != "libx264" && vc != "h264_qsv" && vc != "h264_nvenc" &&
            vc != "hevc" && vc != "h265" && vc != "libx265" && vc != "hevc_qsv" && vc != "hevc_nvenc")
            return "streams." + s.level + ".videoCodec invalid: " + s.videoCodec;

        if (!bitrateValid(s.videoBitrate))
            return "streams." + s.level + ".videoBitrate is invalid: " + s.videoBitrate;
        if (!bitrateValid(s.videoMaxrate))
            return "streams." + s.level + ".videoMaxrate is invalid: " + s.videoMaxrate;

        if (!isPositiveEven(s.portraitWidth) || !isPositiveEven(s.portraitHeight) ||
            !isPositiveEven(s.landscapeWidth) || !isPositiveEven(s.landscapeHeight))
            return "streams." + s.level + " dimensions must be positive even numbers";
    }

    return ""; // OK
}

std::vector<StreamConfig> ConfigManager::DefaultStreams() {
    return {
        {
            "bottom", "{siteName}_audio", true,
            "", "", "", 0, 0, 0, 0,
            "aac", "128k"
        },
        {
            "economic", "{siteName}_economic", false,
            "h264", "400k", "600k", 360, 640, 640, 360,
            "aac", "128k"
        },
        {
            "standard_hevc", "{siteName}_standard_hevc", false,
            "hevc", "600k", "1000k", 720, 1280, 1280, 720,
            "aac", "128k"
        },
        {
            "standard", "{siteName}_standard", false,
            "h264", "1000k", "1500k", 720, 1280, 1280, 720,
            "aac", "128k"
        },
        {
            "high", "{siteName}", false,
            "h264", "2000k", "3000k", 1080, 1920, 1920, 1080,
            "aac", "128k"
        },
    };
}

std::vector<StreamConfig> ConfigManager::ActiveStreams() const {
    std::vector<StreamConfig> active;
    for (auto& s : cfg_.streams) {
        if (!s.audioOnly)
            active.push_back(s);
    }
    return active;
}
