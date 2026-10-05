#include "stream-ladder.hpp"

#include "utils.hpp"

#include <algorithm>
#include <cstdio>

// ── StreamLadder ───────────────────────────────────────────────────

void StreamLadder::LoadFromConfig(const MultipusherConfig& cfg) {
    std::lock_guard<std::mutex> lock(*mu_);
    cfg_ = cfg;

    // Build active (non-audio-only) streams
    activeStreams_.clear();
    for (auto& s : cfg_.streams) {
        if (!s.audioOnly)
            activeStreams_.push_back(s);
    }

    // Initialize output statuses
    outputs_.clear();
    MP_LOG(LOG_INFO, "[obs-multipusher] building SRT URLs: site=%s host=%s:%d app=%s",
           cfg_.siteName.c_str(), cfg_.tencentSrt.host.c_str(),
           cfg_.tencentSrt.port, cfg_.tencentSrt.app.c_str());

    for (auto& s : cfg_.streams) {
        OutputStatus os;
        os.level      = s.level;
        os.streamName = SrtAuth::ResolveStreamName(cfg_.siteName, s.streamName);
        os.audioOnly  = s.audioOnly;
        os.codec      = s.audioOnly ? ShortCodec(s.audioCodec) : ShortCodec(s.videoCodec);
        os.status     = "Idle";

        if (s.audioOnly) {
            os.resolution = "audio-only";
            os.bitrate    = s.audioBitrate;
        } else {
            int w = s.portraitWidth, h = s.portraitHeight;
            if (cfg_.input.videoLayout == "landscape") {
                w = s.landscapeWidth;
                h = s.landscapeHeight;
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "%dx%d", w, h);
            os.resolution = buf;
            os.bitrate    = s.videoBitrate;
        }

        // Build SRT URL
        os.srtURL = SrtAuth::BuildSRTURL(
            cfg_.tencentSrt.host,
            cfg_.tencentSrt.port,
            cfg_.tencentSrt.app,
            os.streamName,
            cfg_.tencentSrt.tokenDays
        );

        // Log every stream URL (txSecret masked)
        if (os.audioOnly) {
            MP_LOG(LOG_INFO, "[obs-multipusher]   [%s] %s audio-only (not pushed)",
                   os.level.c_str(), os.streamName.c_str());
        } else {
            MP_LOG(LOG_INFO, "[obs-multipusher]   [%s] %s %s/%s %s → %s",
                   os.level.c_str(), os.streamName.c_str(),
                   os.codec.c_str(), os.bitrate.c_str(), os.resolution.c_str(),
                   Utils::MaskTxSecret(os.srtURL).c_str());
        }

        outputs_.push_back(os);
    }
}

std::vector<OutputStatus> StreamLadder::GetStatuses() const {
    std::lock_guard<std::mutex> lock(*mu_);
    return outputs_;
}

void StreamLadder::SetStatus(const std::string& streamName, const std::string& status) {
    std::lock_guard<std::mutex> lock(*mu_);
    for (auto& os : outputs_) {
        if (os.streamName == streamName) {
            os.status = status;
        }
    }
}

void StreamLadder::SetAllStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(*mu_);
    for (auto& os : outputs_)
        os.status = status;
}

std::string StreamLadder::GetSRTURL(const std::string& level) const {
    std::lock_guard<std::mutex> lock(*mu_);
    for (auto& os : outputs_) {
        if (os.level == level)
            return os.srtURL;
    }
    return "";
}

std::map<std::string, std::string> StreamLadder::ResolvedStreamNames() const {
    std::lock_guard<std::mutex> lock(*mu_);
    std::map<std::string, std::string> result;
    for (auto& os : outputs_) {
        result[os.level] = os.streamName;
    }
    return result;
}

std::vector<std::string> StreamLadder::BuildAllSRTURLs() const {
    std::lock_guard<std::mutex> lock(*mu_);
    std::vector<std::string> urls;
    for (auto& os : outputs_) {
        if (!os.audioOnly)
            urls.push_back(os.srtURL);
    }
    return urls;
}

StreamLadder StreamLadder::Defaults() {
    StreamLadder ladder;
    MultipusherConfig cfg;
    cfg.siteName = "3drush-fwh";
    cfg.streams = ConfigManager::DefaultStreams();
    ladder.LoadFromConfig(cfg);
    return ladder;
}

std::string StreamLadder::ShortCodec(const std::string& codec) {
    std::string c = codec;
    std::transform(c.begin(), c.end(), c.begin(), ::tolower);
    if (c == "h264" || c == "libx264" || c == "h264_qsv" || c == "h264_nvenc" || c == "h264_amf") return "H.264";
    if (c == "hevc" || c == "h265" || c == "libx265" || c == "hevc_qsv" || c == "hevc_nvenc" || c == "hevc_amf") return "HEVC";
    if (c == "libopus" || c == "opus") return "Opus";
    if (c == "aac") return "AAC";
    return codec;
}

std::string StreamLadder::ViewNameForStream(const std::string& streamName) {
    if (streamName.size() >= 4) {
        std::string suffix = streamName.substr(streamName.size() - 3);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
        if (suffix == "fwh" || suffix == "-fw" || suffix == "_fw") return "fwh";
        if (suffix == "fwv") return "fwv";
    }
    // Check for -fwh / -fwv
    for (auto& suffix : {"-fwh", "_fwh", "-fwv", "_fwv"}) {
        if (streamName.size() >= strlen(suffix)) {
            auto endPart = streamName.substr(streamName.size() - strlen(suffix));
            std::string lowerEnd = endPart;
            std::transform(lowerEnd.begin(), lowerEnd.end(), lowerEnd.begin(), ::tolower);
            std::string lowerSuffix = suffix;
            std::transform(lowerSuffix.begin(), lowerSuffix.end(), lowerSuffix.begin(), ::tolower);
            if (lowerEnd == lowerSuffix) {
                if (lowerSuffix.find("fwh") != std::string::npos) return "fwh";
                if (lowerSuffix.find("fwv") != std::string::npos) return "fwv";
            }
        }
    }
    return "fwh"; // default
}

std::string StreamLadder::SiteNameForStream(const std::string& streamName) {
    std::string lower = streamName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("gsp2w") != std::string::npos) return "studio_gsp2w";
    if (lower.find("3drush") != std::string::npos) return "studio_3drush";
    return streamName; // fallback
}
