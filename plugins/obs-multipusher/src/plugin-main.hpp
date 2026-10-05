#pragma once

#include "config-manager.hpp"
#include "stream-ladder.hpp"
#include "status-reporter.hpp"
#include "output-manager.hpp"

#include <obs-module.h>
#include <string>
#include <memory>

#define PLUGIN_NAME    "obs-multipusher"
#define PLUGIN_VERSION "1.1.4"
#define PLUGIN_AUTHOR  "Amor"
#define PLUGIN_DESC    "Multi-stream ABR SRT publisher for Tencent Cloud Live. " \
                       "Encodes one source into four ABR streams and pushes via SRT."

/// Central plugin context holding all subsystems.
/// Accessible globally via g_ctx.
struct MultipusherContext {
    ConfigManager  config;
    StreamLadder   ladder;
    StatusReporter reporter;
    OutputManager  outputs;

    bool publishing = false;
    int qualityBoostPercent = 20;  // quality+ bitrate boost percentage

    /// Load config from default path and sync to subsystems.
    void LoadDefaultConfig();

    /// Fetch push domain from backend API and update config.
    void FetchAndApplyPushDomain();

    /// Apply config to ladder and output manager.
    void ApplyConfig();

    /// Start the publisher (encoders + outputs).
    bool StartPublishing();

    /// Stop the publisher.
    void StopPublishing();

    /// Reload stream ladder from current config.
    void ReloadLadder();

    /// Sync OBS built-in streaming service URL to the main (high) SRT URL.
    void SyncOBSServiceURL();
};

/// Global plugin context.
extern std::unique_ptr<MultipusherContext> g_ctx;
