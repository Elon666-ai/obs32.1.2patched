#pragma once

#include "config-manager.hpp"
#include "srt-auth.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

/// Represents a single ABR output's current status.
struct OutputStatus {
    std::string level;         // "high", "standard", etc.
    std::string streamName;    // resolved stream name (e.g. "3drush-fwh_economic")
    std::string srtURL;        // full SRT publish URL
    std::string status;        // "Idle", "Starting", "Running", "Retrying", "Stopped"
    std::string codec;         // "H.264" / "HEVC" / "AAC"
    std::string resolution;    // "1920x1080"
    std::string bitrate;       // "2000k"
    bool        audioOnly = false;
};

/// Manages the multi-stream ABR ladder: resolution, bitrate, and SRT URL generation
/// for all four (non-audio) output levels.
class StreamLadder {
public:
    StreamLadder() = default;

    /// Initialize from config.
    void LoadFromConfig(const MultipusherConfig& cfg);

    /// Get all output statuses (4 video outputs + optionally audio).
    std::vector<OutputStatus> GetStatuses() const;

    /// Update status for a specific stream.
    void SetStatus(const std::string& streamName, const std::string& status);

    /// Set all statuses to the same value.
    void SetAllStatus(const std::string& status);

    /// Get the SRT URL for a specific stream level.
    std::string GetSRTURL(const std::string& level) const;

    /// Build resolved stream names map (level -> resolved name).
    std::map<std::string, std::string> ResolvedStreamNames() const;

    /// Resolve all SRT URLs for the active streams.
    std::vector<std::string> BuildAllSRTURLs() const;

    /// Active (non-audio-only) stream configs.
    const std::vector<StreamConfig>& ActiveStreams() const { return activeStreams_; }

    /// Current config reference.
    const MultipusherConfig& Config() const { return cfg_; }

    /// Default stream ladder (5 levels).
    static StreamLadder Defaults();

    /// Short human-readable codec name.
    static std::string ShortCodec(const std::string& codec);

    /// View name from stream name (fwh / fwv).
    static std::string ViewNameForStream(const std::string& streamName);

    /// Site name from stream name (studio_3drush / studio_gsp2w).
    static std::string SiteNameForStream(const std::string& streamName);

private:
    MultipusherConfig        cfg_;
    std::vector<StreamConfig> activeStreams_;
    std::vector<OutputStatus> outputs_;
    mutable std::unique_ptr<std::mutex> mu_ = std::make_unique<std::mutex>();
};
