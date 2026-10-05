#pragma once

#include "config-manager.hpp"
#include "stream-ladder.hpp"
#include "srt-auth.hpp"

#include <obs-module.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

/// Callback type for output status changes.
using OutputStatusCallback = std::function<void(const std::string& streamName,
                                                 const std::string& status)>;

/// Manages the full OBS output pipeline for multi-stream ABR:
///   Source → [Scaler] → VideoEncoder → Output → SRT URL
///   Source → AudioEncoder → Output → SRT URL
///
/// Creates one video encoder + one output per ABR level (4 levels),
/// and one shared audio encoder.
class OutputManager {
public:
    OutputManager();
    ~OutputManager();

    /// Configure from MultipusherConfig and StreamLadder.
    /// Must be called before Start().
    void Configure(const MultipusherConfig& cfg, const StreamLadder& ladder);

    /// Start all outputs. Returns false if any fail.
    bool Start();

    /// Stop all outputs gracefully.
    void Stop();

    /// Whether outputs are currently running.
    bool IsRunning() const { return running_; }

    /// Set quality boost bitrate percentage (0-100) applied to all encoders.
    void SetQualityBoost(int percent) { qualityBoostPercent_ = percent; }

    /// Set status change callback (for UI / reporter integration).
    void SetStatusCallback(OutputStatusCallback cb) { statusCallback_ = std::move(cb); }

    /// Get current status map: streamName → status
    std::map<std::string, std::string> GetStatusSnapshot() const;

    /// Set the OBS video source to encode.
    void SetVideoSource(obs_source_t* source);

    /// Set the OBS audio source (optional; uses global audio if null).
    void SetAudioSource(obs_source_t* source);

private:
    struct OutputSlot {
        std::string level;
        std::string streamName;
        std::string srtURL;

        StreamConfig    streamCfg;
        obs_encoder_t*  videoEncoder = nullptr;
        obs_encoder_t*  audioEncoder = nullptr;
        obs_output_t*   output       = nullptr;
        std::string      status;
        int              reconnectCount = 0;
        OutputManager*  manager      = nullptr;  // back-pointer for signal callbacks
    };

    // ── Internal helpers ────────────────────────────────────────
    void createEncoders();
    void createOutputs();
    void destroyAll();
    void disconnectOutputSignals();

    obs_encoder_t* createVideoEncoder(const StreamConfig& sc);
    obs_encoder_t* createAudioEncoder(const StreamConfig& sc);
    obs_output_t*  createSRTOutput(const std::string& name,
                                    const std::string& srtURL);

    static void onOutputStart(void* data, calldata_t* cd);
    static void onOutputStop(void* data, calldata_t* cd);
    static void onOutputReconnect(void* data, calldata_t* cd);
    static void onOutputReconnectSuccess(void* data, calldata_t* cd);

    void emitStatus(const std::string& streamName, const std::string& status);

    // ── State ───────────────────────────────────────────────────
    MultipusherConfig config_;
    std::vector<OutputSlot> slots_;

    obs_source_t*  videoSource_  = nullptr;
    obs_source_t*  audioSource_  = nullptr;

    int qualityBoostPercent_{0};
    std::atomic<bool> running_{false};
    OutputStatusCallback statusCallback_;

    // Signal handlers (must outlive outputs)
    std::vector<signal_handler_t*> signalHandlers_;
};
