#pragma once

#include <string>
#include <vector>
#include <cstdint>

/// Stream configuration for one ABR level.
struct StreamConfig {
    std::string level;            // "high", "standard", "standard_hevc", "economic", "bottom"
    std::string streamName;       // "{siteName}" or "{siteName}_economic" etc.
    bool        audioOnly = false;

    std::string videoCodec;       // "h264" / "hevc"
    std::string videoBitrate;     // "2000k"
    std::string videoMaxrate;     // "3000k"
    int         portraitWidth  = 1080;
    int         portraitHeight = 1920;
    int         landscapeWidth  = 1920;
    int         landscapeHeight = 1080;

    std::string audioCodec;       // "aac"
    std::string audioBitrate;     // "128k"
};

/// Tencent SRT connection settings.
struct TencentSrtConfig {
    std::string host = "publish.numericgame.ph";
    int         port = 9000;
    std::string app  = "live";
    int         tokenDays = 90;
};

/// Input source configuration.
struct InputConfig {
    std::string mode;             // "device" or "file"
    std::string videoDevice;
    std::string audioDevice;
    std::string videoFile;
    std::string videoLayout = "portrait"; // "portrait" or "landscape"
};

/// Publish behavior configuration.
struct PublishConfig {
    bool onReady             = false;
    int  reconnectMinSeconds = 3;
    int  reconnectMaxSeconds = 60;
};

/// Complete multipusher configuration.
struct MultipusherConfig {
    std::string siteName;         // "3drush-fwh"
    std::string backendURL;       // ABR player backend base URL (e.g. http://127.0.0.1:8080)
    TencentSrtConfig  tencentSrt;
    InputConfig       input;
    std::vector<StreamConfig> streams;
    PublishConfig     publish;
};

/// Configuration manager: load / save / validate.
/// Uses OBS obs_data_t for JSON serialization — no external YAML dependency.
class ConfigManager {
public:
    ConfigManager();

    /// Resolve default config file path.
    static std::string DefaultPath();

    /// Load configuration from a JSON file. Returns true on success.
    bool Load(const std::string& path);

    /// Save current configuration to a JSON file. Returns true on success.
    bool Save(const std::string& path) const;

    /// Validate the current configuration. Returns empty string if valid,
    /// otherwise an error message.
    std::string Validate() const;

    /// Access the configuration data.
    const MultipusherConfig& Get() const { return cfg_; }
    MultipusherConfig& GetMutable() { return cfg_; }
    void Set(const MultipusherConfig& cfg) { cfg_ = cfg; }

    /// Default stream ladder (5 levels).
    static std::vector<StreamConfig> DefaultStreams();

    /// Active (non-audio-only) streams for ABR output.
    std::vector<StreamConfig> ActiveStreams() const;

    /// Current config file path.
    const std::string& Path() const { return path_; }
    void SetPath(const std::string& p) { path_ = p; }

private:
    MultipusherConfig cfg_;
    std::string       path_;
};
