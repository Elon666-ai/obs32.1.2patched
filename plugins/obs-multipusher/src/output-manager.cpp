#include "output-manager.hpp"
#include "utils.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdio>

// ── OutputManager ─────────────────────────────────────────────────

OutputManager::OutputManager() = default;

OutputManager::~OutputManager() {
    Stop();
}

void OutputManager::Configure(const MultipusherConfig& cfg, const StreamLadder& ladder) {
    Stop(); // stop any running outputs first

    config_ = cfg;

    // Build output slots from active (non-audio-only) streams
    slots_.clear();
    auto statuses = ladder.GetStatuses();

    for (auto& os : statuses) {
        if (os.audioOnly) continue; // skip audio-only

        // Find matching StreamConfig
        StreamConfig sc;
        bool found = false;
        for (auto& s : cfg.streams) {
            if (s.level == os.level) {
                sc = s;
                found = true;
                break;
            }
        }
        if (!found) continue;

        OutputSlot slot;
        slot.level      = os.level;
        slot.streamName = os.streamName;
        slot.srtURL     = os.srtURL;
        slot.streamCfg  = sc;
        slot.status     = "Idle";
        slot.manager    = this;        // back-pointer for signal callbacks
        slots_.push_back(std::move(slot));
    }

    MP_LOG(LOG_INFO, "[obs-multipusher] output-manager configured: %zu output slots", slots_.size());
}

void OutputManager::SetVideoSource(obs_source_t* source) {
    videoSource_ = source;
}

void OutputManager::SetAudioSource(obs_source_t* source) {
    audioSource_ = source;
}

// Apply quality boost percentage to a kbps bitrate value.
static inline int applyQualityBoost(int kbps, int boostPercent) {
    if (boostPercent <= 0) return kbps;
    return static_cast<int>(kbps * (100 + boostPercent) / 100);
}

bool OutputManager::Start() {
    if (running_) {
        MP_LOG(LOG_WARNING, "[obs-multipusher] output-manager already running");
        return false;
    }
    if (slots_.empty()) {
        MP_LOG(LOG_ERROR, "[obs-multipusher] no output slots configured");
        return false;
    }

    // ── 1. Create encoders ──────────────────────────────────────
    createEncoders();

    // ── 2. Create outputs ───────────────────────────────────────
    createOutputs();

    // ── 3. Start all outputs ────────────────────────────────────
    bool anyStarted = false;
    for (auto& slot : slots_) {
        if (!slot.output || !slot.videoEncoder || !slot.audioEncoder) {
            MP_LOG(LOG_ERROR, "[obs-multipusher] slot %s missing output or encoder, skipping",
                   slot.level.c_str());
            continue;
        }

        // Try explicit encoder init before start
        {
            auto* venc = obs_output_get_video_encoder(slot.output);
            auto* aenc = obs_output_get_audio_encoder(slot.output, 0);
            MP_LOG(LOG_INFO, "[obs-multipusher] pre-start [%s]: venc=%s aenc=%s",
                   slot.level.c_str(),
                   venc ? "ok" : "null",
                   aenc ? "ok" : "null");
        }

        // Mark "Starting" BEFORE obs_output_start(): that call runs the output's
        // start callback synchronously, which fires the "start" signal →
        // onOutputStart → status "Running". Setting "Starting" *after* the call
        // (as before) overwrote that "Running", so the UI was stuck on "Starting".
        slot.status = "Starting";
        emitStatus(slot.streamName, "Starting");

        if (obs_output_start(slot.output)) {
            anyStarted = true;
            MP_LOG(LOG_INFO, "[obs-multipusher] started output: %s → %s",
                   slot.level.c_str(), Utils::MaskTxSecret(slot.srtURL).c_str());
        } else {
            const char* err = obs_output_get_last_error(slot.output);
            MP_LOG(LOG_ERROR, "[obs-multipusher] failed to start output %s: %s",
                   slot.level.c_str(), err ? err : "unknown error");

            // If the video encoder is hardware-accelerated (NVENC/QSV/AMF) and its
            // initialization failed (a common symptom on machines where the GPU driver
            // is misconfigured, outdated, or the GPU is in a low-power state), fall
            // back to software x264 and retry once.
            bool retried = false;
            if (slot.videoEncoder) {
                const char* encId = obs_encoder_get_id(slot.videoEncoder);
                std::string encIdStr = encId ? encId : "";
                bool isHw = (encIdStr.find("nvenc") != std::string::npos) ||
                            (encIdStr.find("qsv")   != std::string::npos) ||
                            (encIdStr.find("amf")   != std::string::npos);
                if (isHw) {
                    MP_LOG(LOG_WARNING, "[obs-multipusher] hardware encoder %s failed to init for %s, falling back to obs_x264",
                           encIdStr.c_str(), slot.level.c_str());

                    // Destroy hardware encoder, create software x264 replacement
                    obs_encoder_release(slot.videoEncoder);
                    slot.videoEncoder = nullptr;

                    slot.videoEncoder = obs_video_encoder_create(
                        "obs_x264", (slot.level + "_venc").c_str(), nullptr, nullptr);
                    if (slot.videoEncoder) {
                        // Apply same bitrate settings
                        obs_data_t* s = obs_data_create();
                        int bitrateKbps = 2000;
                        std::string br = slot.streamCfg.videoBitrate;
                        if (!br.empty() && (br.back() == 'k' || br.back() == 'K')) {
                            br.pop_back();
                            bitrateKbps = std::stoi(br);
                        }
                        bitrateKbps = applyQualityBoost(bitrateKbps, qualityBoostPercent_);
                        obs_data_set_string(s, "rate_control", "CBR");
                        obs_data_set_int(s, "bitrate", bitrateKbps);
                        obs_data_set_int(s, "keyint_sec", 2);
                        obs_data_set_string(s, "preset", "veryfast");
                        obs_data_set_string(s, "profile", "high");
                        obs_data_set_int(s, "bf", 0);
                        obs_encoder_update(slot.videoEncoder, s);
                        obs_data_release(s);

                        obs_encoder_set_video(slot.videoEncoder, obs_get_video());

                        // Re-scale to the configured resolution
                        int width = slot.streamCfg.portraitWidth;
                        int height = slot.streamCfg.portraitHeight;
                        if (config_.input.videoLayout == "landscape") {
                            width  = slot.streamCfg.landscapeWidth;
                            height = slot.streamCfg.landscapeHeight;
                        }
                        if (width > 0 && height > 0) {
                            obs_encoder_set_scaled_size(slot.videoEncoder,
                                static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                            obs_encoder_set_gpu_scale_type(slot.videoEncoder, OBS_SCALE_BICUBIC);
                        }

                        // Re-attach encoder to output and retry
                        obs_output_set_video_encoder(slot.output, slot.videoEncoder);

                        MP_LOG(LOG_INFO, "[obs-multipusher] retrying %s with software encoder...",
                               slot.level.c_str());
                        if (obs_output_start(slot.output)) {
                            anyStarted = true;
                            retried = true;
                            MP_LOG(LOG_INFO, "[obs-multipusher] started output (sw fallback): %s → %s",
                                   slot.level.c_str(), Utils::MaskTxSecret(slot.srtURL).c_str());
                        } else {
                            const char* err2 = obs_output_get_last_error(slot.output);
                            MP_LOG(LOG_ERROR, "[obs-multipusher] software fallback also failed for %s: %s",
                                   slot.level.c_str(), err2 ? err2 : "unknown error");
                        }
                    } else {
                        MP_LOG(LOG_ERROR, "[obs-multipusher] failed to create software encoder for %s",
                               slot.level.c_str());
                    }
                }
            }

            if (!retried) {
                slot.status = "Stopped";
                emitStatus(slot.streamName, "Stopped");
            }
        }
    }

    if (anyStarted) {
        running_ = true;
        return true;
    } else {
        destroyAll();
        return false;
    }
}

void OutputManager::Stop() {
    if (!running_) return;
    running_ = false;

    MP_LOG(LOG_INFO, "[obs-multipusher] stopping all outputs...");

    // Disconnect the status-signal callbacks FIRST. obs_output_stop() is
    // asynchronous: the "stop"/"reconnect" signal fires later on the output's
    // own thread and would call our callback with a pointer to an OutputSlot we
    // are about to free in destroyAll() — a use-after-free. signal_handler
    // dispatch and signal_handler_disconnect share the handler mutex, so once
    // disconnect returns no callback is in-flight or can fire afterwards.
    disconnectOutputSignals();

    for (auto& slot : slots_) {
        if (slot.output && obs_output_active(slot.output)) {
            obs_output_stop(slot.output);
            MP_LOG(LOG_INFO, "[obs-multipusher] stopped output: %s", slot.level.c_str());
        }
    }

    destroyAll();

    MP_LOG(LOG_INFO, "[obs-multipusher] all outputs stopped");
}

std::map<std::string, std::string> OutputManager::GetStatusSnapshot() const {
    std::map<std::string, std::string> result;
    for (auto& slot : slots_) {
        result[slot.streamName] = slot.status;
    }
    return result;
}

// ── Encoder creation ────────────────────────────────────────────

void OutputManager::createEncoders() {
    // Destroy existing encoders first
    for (auto& slot : slots_) {
        if (slot.videoEncoder) {
            obs_encoder_release(slot.videoEncoder);
            slot.videoEncoder = nullptr;
        }
        if (slot.audioEncoder) {
            obs_encoder_release(slot.audioEncoder);
            slot.audioEncoder = nullptr;
        }
    }
    signalHandlers_.clear();

    // One video + one dedicated audio encoder per output slot
    for (auto& slot : slots_) {
        slot.videoEncoder = createVideoEncoder(slot.streamCfg);
        if (!slot.videoEncoder) {
            MP_LOG(LOG_ERROR, "[obs-multipusher] failed to create video encoder for %s",
                   slot.level.c_str());
        }
        slot.audioEncoder = createAudioEncoder(slot.streamCfg);
        if (!slot.audioEncoder) {
            MP_LOG(LOG_ERROR, "[obs-multipusher] failed to create audio encoder for %s",
                   slot.level.c_str());
        }
    }
}

// Helper: check if an encoder type exists by iterating registered types
static bool encoderTypeExists(const char* id) {
    size_t idx = 0;
    const char* typeId;
    while (obs_enum_encoder_types(idx, &typeId)) {
        if (typeId && strcmp(typeId, id) == 0)
            return true;
        ++idx;
    }
    return false;
}

obs_encoder_t* OutputManager::createVideoEncoder(const StreamConfig& sc) {
    // Determine which encoder ID to use
    std::string encoderId;
    std::string codec = Utils::ToLower(sc.videoCodec);

    if (codec == "h264" || codec == "libx264" || codec == "h264_qsv" || codec == "h264_nvenc") {
        // Priority: NVIDIA NVENC (OBS 30+ texture encoder) → Intel QSV v2 → AMD AMF → x264 (software)
        // Only probe IDs that OBS actually registers; avoid stale names whose
        // encoderTypeExists() may return true but that fail at init time.
        encoderId = "obs_x264"; // safe default
        if (encoderTypeExists("obs_nvenc_h264_tex"))
            encoderId = "obs_nvenc_h264_tex";
        else if (encoderTypeExists("obs_qsv11_v2"))
            encoderId = "obs_qsv11_v2";
        else if (encoderTypeExists("h264_texture_amf"))
            encoderId = "h264_texture_amf";
    } else if (codec == "hevc" || codec == "h265" || codec == "libx265" || codec == "hevc_qsv" || codec == "hevc_nvenc") {
        // Priority: NVIDIA HEVC (OBS 30+) → Intel QSV HEVC → AMD AMF → H.264 fallback
        if (encoderTypeExists("obs_nvenc_hevc_tex"))
            encoderId = "obs_nvenc_hevc_tex";
        else if (encoderTypeExists("obs_qsv11_hevc"))
            encoderId = "obs_qsv11_hevc";
        else if (encoderTypeExists("h265_texture_amf"))
            encoderId = "h265_texture_amf";
        else
            encoderId = "obs_x264"; // fallback to H.264
    }

    obs_encoder_t* encoder = obs_video_encoder_create(
        encoderId.c_str(),
        (sc.level + "_venc").c_str(),
        nullptr,  // settings filled below
        nullptr   // hotkey data
    );

    if (!encoder) {
        MP_LOG(LOG_WARNING, "[obs-multipusher] encoder '%s' not found for level %s, falling back to obs_x264",
               encoderId.c_str(), sc.level.c_str());
        encoder = obs_video_encoder_create("obs_x264", (sc.level + "_venc").c_str(), nullptr, nullptr);
        if (!encoder) return nullptr;
    }

    // Configure encoder settings
    obs_data_t* settings = obs_data_create();

    // Bitrate
    obs_data_set_string(settings, "rate_control", "CBR");
    int bitrateKbps = 2000;
    std::string br = sc.videoBitrate;
    if (!br.empty() && (br.back() == 'k' || br.back() == 'K')) {
        br.pop_back();
        bitrateKbps = std::stoi(br);
    }
    bitrateKbps = applyQualityBoost(bitrateKbps, qualityBoostPercent_);
    obs_data_set_int(settings, "bitrate", bitrateKbps);

    // Keyframe interval (2 seconds at assumed 30fps → 60)
    obs_data_set_int(settings, "keyint_sec", 2);

    // Preset
    obs_data_set_string(settings, "preset", "veryfast");

    // Profile (only for H.264)
    if (encoderId.find("x264") != std::string::npos)
        obs_data_set_string(settings, "profile", "high");

    // BFrames
    obs_data_set_int(settings, "bf", 0);

    obs_encoder_update(encoder, settings);
    obs_data_release(settings);

    // Set video source on encoder (always use global OBS video)
    obs_encoder_set_video(encoder, obs_get_video());

    MP_LOG(LOG_INFO, "[obs-multipusher] created video encoder: %s (%s) bitrate=%dkbps%s",
           sc.level.c_str(), encoderId.c_str(), bitrateKbps,
           qualityBoostPercent_ > 0
               ? (" (quality+" + std::to_string(qualityBoostPercent_) + "%)").c_str()
               : "");

    return encoder;
}

obs_encoder_t* OutputManager::createAudioEncoder(const StreamConfig& sc) {
    const char* encoderId = nullptr;

    // Check available AAC encoders
    if (encoderTypeExists("CoreAudio_AAC"))
        encoderId = "CoreAudio_AAC";
    else if (encoderTypeExists("ffmpeg_aac"))
        encoderId = "ffmpeg_aac";
    else
        return nullptr;

    obs_encoder_t* encoder = obs_audio_encoder_create(encoderId,
        ("mp_aenc_" + sc.level).c_str(), nullptr, 0, nullptr);
    if (!encoder) return nullptr;

    obs_data_t* settings = obs_data_create();
    int audioBitrate = 128; // default
    std::string abr = sc.audioBitrate;
    if (!abr.empty() && (abr.back() == 'k' || abr.back() == 'K')) {
        abr.pop_back();
        audioBitrate = std::stoi(abr);
    }
    obs_data_set_int(settings, "bitrate", audioBitrate);
    obs_encoder_update(encoder, settings);
    obs_data_release(settings);

    // Set audio source on encoder (always use global OBS audio)
    obs_encoder_set_audio(encoder, obs_get_audio());

    MP_LOG(LOG_INFO, "[obs-multipusher] created audio encoder: %s bitrate=%dkbps",
           encoderId, audioBitrate);

    return encoder;
}

// ── Output creation ─────────────────────────────────────────────

void OutputManager::createOutputs() {
    for (auto& slot : slots_) {
        if (slot.output) {
            obs_output_release(slot.output);
            slot.output = nullptr;
        }

        std::string outputName = "mp_" + slot.level;
        slot.output = createSRTOutput(outputName, slot.srtURL);
        if (!slot.output) {
            MP_LOG(LOG_ERROR, "[obs-multipusher] failed to create SRT output for %s",
                   slot.level.c_str());
            continue;
        }

        // ── Set encoders on output ──────────────────────────────
        if (slot.videoEncoder)
            obs_output_set_video_encoder(slot.output, slot.videoEncoder);
        if (slot.audioEncoder)
            obs_output_set_audio_encoder(slot.output, slot.audioEncoder, 0);

        // ── Set video resolution via encoder scaled size ───────
        int width = slot.streamCfg.portraitWidth;
        int height = slot.streamCfg.portraitHeight;
        if (config_.input.videoLayout == "landscape") {
            width  = slot.streamCfg.landscapeWidth;
            height = slot.streamCfg.landscapeHeight;
        }

        if (width > 0 && height > 0 && slot.videoEncoder) {
            obs_encoder_set_scaled_size(slot.videoEncoder,
                                        static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height));
            obs_encoder_set_gpu_scale_type(slot.videoEncoder, OBS_SCALE_BICUBIC);

            MP_LOG(LOG_INFO, "[obs-multipusher] scaled output %s to %dx%d",
                   slot.level.c_str(), width, height);
        }

        // ── Register signal callbacks (pass slot ptr as data) ───
        signal_handler_t* handler = obs_output_get_signal_handler(slot.output);
        if (handler) {
            signal_handler_connect(handler, "start", onOutputStart, &slot);
            signal_handler_connect(handler, "stop", onOutputStop, &slot);
            signal_handler_connect(handler, "reconnect", onOutputReconnect, &slot);
            signal_handler_connect(handler, "reconnect_success", onOutputReconnectSuccess, &slot);
            signalHandlers_.push_back(handler);
        }

        // Enable libobs' reconnect machinery. When the SRT link dies,
        // srt_pusher stops the output with OBS_OUTPUT_DISCONNECTED; libobs then
        // tears it down and re-runs the output's start callback with
        // exponential backoff (see output_reconnect()/reconnect_thread() in
        // obs-output.c). The attempt count is deliberately huge so a 24/7
        // stream keeps retrying; the delay starts at reconnectMinSeconds and
        // grows by 1.5x per attempt (capped at 15 minutes by libobs).
        obs_output_set_reconnect_settings(slot.output, 100000,
                                          std::max(1, config_.publish.reconnectMinSeconds));

        MP_LOG(LOG_INFO, "[obs-multipusher] created SRT output: %s → %dx%d → %s",
               slot.level.c_str(), width, height,
               Utils::MaskTxSecret(slot.srtURL).c_str());
    }
}

obs_output_t* OutputManager::createSRTOutput(const std::string& name,
                                              const std::string& srtURL) {
    // ffmpeg_muxer reads "path", ffmpeg_mpegts_muxer reads "url"
    // Set both so either type works
    auto makeSettings = [&]() {
        obs_data_t* s = obs_data_create();
        obs_data_set_string(s, "path", srtURL.c_str());
        obs_data_set_string(s, "url", srtURL.c_str());
        obs_data_set_string(s, "muxer_settings",
                            "mpegts_flags=+resend_headers+system_b");
        return s;
    };

    // ffmpeg_muxer first: encoded+noservice, reads "path"
    const char* outputTypes[] = {"mp_srt_pusher", "ffmpeg_muxer",
                                  "ffmpeg_mpegts_muxer", nullptr};

    for (int i = 0; outputTypes[i]; ++i) {
        obs_data_t* settings = makeSettings();
        obs_output_t* output = obs_output_create(outputTypes[i], name.c_str(),
                                                  settings, nullptr);
        obs_data_release(settings);
        if (!output) continue;

        uint32_t flags = obs_output_get_flags(output);
        if (!(flags & OBS_OUTPUT_ENCODED)) {
            obs_output_release(output);
            continue;
        }

        // Attach service if required
        if ((flags & OBS_OUTPUT_SERVICE) && !obs_output_get_service(output)) {
            obs_data_t* svcS = obs_data_create();
            obs_data_set_string(svcS, "server", srtURL.c_str());
            obs_data_set_bool(svcS, "use_auth", false);
            obs_data_set_bool(svcS, "bwtest", false);
            obs_service_t* svc = obs_service_create("rtmp_custom",
                ("mp_svc_" + name).c_str(), svcS, nullptr);
            obs_data_release(svcS);
            if (svc) { obs_output_set_service(output, svc); obs_service_release(svc); }
        }

        MP_LOG(LOG_INFO, "[obs-multipusher] using output type: %s", outputTypes[i]);
        return output;
    }
    return nullptr;
}

// ── Cleanup ─────────────────────────────────────────────────────

void OutputManager::destroyAll() {
    // Ensure no signal callback can fire on a slot we're freeing (safety net for
    // the Start()-failure path, which reaches destroyAll() without Stop()).
    disconnectOutputSignals();

    for (auto& slot : slots_) {
        if (slot.output) {
            obs_output_release(slot.output);
            slot.output = nullptr;
        }
        if (slot.videoEncoder) {
            obs_encoder_release(slot.videoEncoder);
            slot.videoEncoder = nullptr;
        }
        if (slot.audioEncoder) {
            obs_encoder_release(slot.audioEncoder);
            slot.audioEncoder = nullptr;
        }
    }
    slots_.clear();
    signalHandlers_.clear();
}

// Disconnect every output signal we connected in createOutputs(). Must be called
// before releasing/clearing slots_ so a late async signal can't dereference a
// freed OutputSlot. Idempotent: disconnecting an already-removed callback is a
// no-op, and the &slot addresses match those used at connect time (slots_ is not
// reallocated between createOutputs() and teardown).
void OutputManager::disconnectOutputSignals() {
    for (auto& slot : slots_) {
        if (!slot.output) continue;
        signal_handler_t* h = obs_output_get_signal_handler(slot.output);
        if (!h) continue;
        signal_handler_disconnect(h, "start", onOutputStart, &slot);
        signal_handler_disconnect(h, "stop", onOutputStop, &slot);
        signal_handler_disconnect(h, "reconnect", onOutputReconnect, &slot);
        signal_handler_disconnect(h, "reconnect_success", onOutputReconnectSuccess, &slot);
    }
    signalHandlers_.clear();
}

// ── Signal callbacks ────────────────────────────────────────────

void OutputManager::onOutputStart(void* data, calldata_t* /*cd*/) {
    auto* slot = static_cast<OutputSlot*>(data);
    slot->status = "Running";
    slot->reconnectCount = 0;  // reset exception counter
    MP_LOG(LOG_INFO, "[obs-multipusher] output running: %s", slot->level.c_str());
    slot->manager->emitStatus(slot->streamName, "Running");
}

void OutputManager::onOutputStop(void* data, calldata_t* /*cd*/) {
    auto* slot = static_cast<OutputSlot*>(data);
    slot->status = "Stopped";
    slot->reconnectCount = 0;
    MP_LOG(LOG_INFO, "[obs-multipusher] output stopped (signal): %s", slot->level.c_str());
    slot->manager->emitStatus(slot->streamName, "Stopped");
}

void OutputManager::onOutputReconnect(void* data, calldata_t* /*cd*/) {
    auto* slot = static_cast<OutputSlot*>(data);
    slot->reconnectCount++;
    // After 3 consecutive reconnect attempts without success, mark as Exception.
    if (slot->reconnectCount >= 3) {
        if (slot->status != "Exception") {
            slot->status = "Exception";
            MP_LOG(LOG_WARNING, "[obs-multipusher] output exception (reconnect storm): %s (count=%d)",
                   slot->level.c_str(), slot->reconnectCount);
            slot->manager->emitStatus(slot->streamName, "Exception");
        }
    } else {
        slot->status = "Retrying";
        slot->manager->emitStatus(slot->streamName, "Retrying");
    }
}

void OutputManager::onOutputReconnectSuccess(void* data, calldata_t* /*cd*/) {
    auto* slot = static_cast<OutputSlot*>(data);
    slot->status = "Running";
    slot->reconnectCount = 0;  // reset on successful reconnect
    slot->manager->emitStatus(slot->streamName, "Running");
}

void OutputManager::emitStatus(const std::string& streamName, const std::string& status) {
    if (statusCallback_) {
        statusCallback_(streamName, status);
    }
}
