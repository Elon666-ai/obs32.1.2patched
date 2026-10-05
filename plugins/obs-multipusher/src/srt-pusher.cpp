#include "srt-pusher.hpp"
#include "utils.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
}

#include <cstdarg>

#include <obs-module.h>
#include <util/platform.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <mutex>

// ── Per-output state ──────────────────────────────────────────────

struct SrtPusher {
    obs_output_t*  output = nullptr;
    AVFormatContext* fmt_ctx = nullptr;

    int  video_stream_idx = -1;
    int  audio_stream_idx = -1;

    bool active     = false;
    bool sent_audio = false;
    bool sent_video = false;

    // Common epoch (in the stream's 90kHz timebase) shared by audio+video so
    // the two stay aligned. Captured from the first packet of either stream.
    int64_t start_ts = -1;

    // Diagnostics
    std::string name;        // output name (e.g. "mp_economic")
    bool logged_v = false;   // logged first video packet bytes
    bool logged_a = false;   // logged first audio packet bytes
    int  werr = 0;           // per-output write-error count
};

// ── FFmpeg log bridge ─────────────────────────────────────────────
// Route libavformat / libsrt diagnostics into the OBS log so connection
// rejects ("Connection was broken", auth failures, non-monotonic dts, …)
// are visible instead of being swallowed by FFmpeg's default stderr sink.
static void srt_pusher_av_log(void*, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;  // errors + warnings only
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, vl);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0) return;
    MP_LOG(level <= AV_LOG_ERROR ? LOG_ERROR : LOG_WARNING,
           "[obs-multipusher] ffmpeg: %s", line);
}

// ── Helpers ───────────────────────────────────────────────────────

static inline int64_t rescale_ts(int64_t val, AVRational src, AVRational dst) {
    return av_rescale_q_rnd(val, src, dst,
        (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

// ── OBS output callbacks ──────────────────────────────────────────

static const char* srt_pusher_get_name(void*) {
    return "SRT Pusher (FFmpeg)";
}

static void* srt_pusher_create(obs_data_t*, obs_output_t* output) {
    auto* sp = new SrtPusher;
    sp->output = output;
    const char* nm = obs_output_get_name(output);
    sp->name = nm ? nm : "";
    MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher created: %s", sp->name.c_str());
    return sp;
}

static void srt_pusher_destroy(void* data) {
    auto* sp = static_cast<SrtPusher*>(data);
    delete sp;
    MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher destroyed");
}

static bool srt_pusher_start(void* data) {
    auto* sp = static_cast<SrtPusher*>(data);
    obs_output_t* output = sp->output;

    // Initialize encoders
    if (!obs_output_can_begin_data_capture(output, 0)) {
        MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: can_begin_data_capture failed");
        return false;
    }
    if (!obs_output_initialize_encoders(output, 0)) {
        const char* err = obs_output_get_last_error(output);
        MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: encoder init failed: %s",
               err ? err : "unknown");
        return false;
    }

    // Get the URL from output settings
    obs_data_t* settings = obs_output_get_settings(output);
    const char* url = obs_data_get_string(settings, "url");
    if (!url || !*url) {
        MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: no URL configured");
        obs_data_release(settings);
        return false;
    }

    MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher: starting push to %s",
           Utils::MaskTxSecret(url).c_str());

    // ── Allocate avformat muxer (mpegts) ──────────────────────
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_alloc_output_context2(&fmt_ctx, nullptr, "mpegts", url);
    if (ret < 0 || !fmt_ctx) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: alloc muxer failed: %s", errbuf);
        obs_data_release(settings);
        return false;
    }

    av_opt_set(fmt_ctx->priv_data, "mpegts_flags", "+resend_headers+system_b", 0);

    // OBS encoders already emit Annex-B (start-code) H.264/HEVC, so NO bitstream
    // conversion is needed. We must disable libavformat's auto-bsf: OBS's H.264
    // QSV packets begin with an extra leading zero ("00 00 00 00 01"), so the
    // muxer's Annex-B test (AV_RB32==1) fails, it wrongly inserts
    // h264_mp4toannexb, that filter reads "00 00 00 00" as a zero-length NAL and
    // rejects every packet with "Invalid data found when processing input".
    // (HEVC packets start with a clean "00 00 00 01" so they were unaffected.)
    fmt_ctx->flags &= ~AVFMT_FLAG_AUTO_BSF;

    // ── Add video stream ───────────────────────────────────────
    obs_encoder_t* venc = obs_output_get_video_encoder(output);
    if (venc) {
        AVStream* vs = avformat_new_stream(fmt_ctx, nullptr);
        if (!vs) {
            MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: failed to create video stream");
            avformat_free_context(fmt_ctx);
            obs_data_release(settings);
            return false;
        }
        vs->id = 0;
        vs->time_base = {1, 90000};  // MPEG-TS default

        // Copy codec parameters from encoder
        obs_data_t* venc_settings = obs_encoder_get_settings(venc);
        // Get extradata (AVC/HEVC headers) from encoder
        uint8_t* extra = nullptr;
        size_t extra_size = 0;
        obs_encoder_get_extra_data(venc, &extra, &extra_size);
        if (extra && extra_size > 0) {
            vs->codecpar->extradata = (uint8_t*)av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(vs->codecpar->extradata, extra, extra_size);
            vs->codecpar->extradata_size = (int)extra_size;
            // DIAG: first bytes reveal Annex-B (00 00 00 01 / 00 00 01) vs AVCC/hvcC (01 ..)
            if (extra_size >= 5)
                MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher[%s]: video extradata[0..4]=%02x %02x %02x %02x %02x size=%zu",
                       sp->name.c_str(), extra[0], extra[1], extra[2], extra[3], extra[4], extra_size);
        }

        // Determine codec from the encoder's CODEC string (not its id).
        // Using the id was a bug: the HEVC encoder id "obs_qsv11_hevc" contains
        // "qsv", which matched the H.264 branch first → HEVC was muxed as H.264.
        const char* codecName = obs_encoder_get_codec(venc);
        AVCodecID codecId = AV_CODEC_ID_H264; // default
        if (codecName) {
            std::string c(codecName);
            if (c.find("hevc") != std::string::npos || c.find("h265") != std::string::npos)
                codecId = AV_CODEC_ID_HEVC;
            else if (c.find("av1") != std::string::npos)
                codecId = AV_CODEC_ID_AV1;
            else
                codecId = AV_CODEC_ID_H264;
        }
        vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vs->codecpar->codec_id = codecId;

        sp->video_stream_idx = vs->index;

        obs_data_release(venc_settings);
        MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher: video stream added codec=%d extrasize=%zu",
               codecId, extra_size);
    }

    // ── Add audio stream ───────────────────────────────────────
    obs_encoder_t* aenc = obs_output_get_audio_encoder(output, 0);
    if (aenc) {
        AVStream* as = avformat_new_stream(fmt_ctx, nullptr);
        if (!as) {
            MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: failed to create audio stream");
            avformat_free_context(fmt_ctx);
            obs_data_release(settings);
            return false;
        }
        as->id = 1;
        as->time_base = {1, 90000};

        uint8_t* extra = nullptr;
        size_t extra_size = 0;
        obs_encoder_get_extra_data(aenc, &extra, &extra_size);
        if (extra && extra_size > 0) {
            as->codecpar->extradata = (uint8_t*)av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(as->codecpar->extradata, extra, extra_size);
            as->codecpar->extradata_size = (int)extra_size;
        }

        as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        as->codecpar->codec_id = AV_CODEC_ID_AAC;
        as->codecpar->sample_rate = 48000;
        as->codecpar->ch_layout = AV_CHANNEL_LAYOUT_STEREO;

        sp->audio_stream_idx = as->index;
        MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher: audio stream added");
    }

    // ── Open SRT URL ───────────────────────────────────────────
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&fmt_ctx->pb, url, AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: avio_open2 failed: %s", errbuf);
            avformat_free_context(fmt_ctx);
            obs_data_release(settings);
            return false;
        }
    }

    // ── Write header ────────────────────────────────────────────
    ret = avformat_write_header(fmt_ctx, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        MP_LOG(LOG_ERROR, "[obs-multipusher] srt_pusher: write_header failed: %s", errbuf);
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        obs_data_release(settings);
        return false;
    }

    sp->fmt_ctx = fmt_ctx;
    sp->active = true;
    sp->sent_audio = false;
    sp->sent_video = false;
    sp->start_ts = -1;

    obs_output_begin_data_capture(output, 0);
    obs_data_release(settings);

    MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher: started successfully");
    return true;
}

static void srt_pusher_stop(void* data, uint64_t) {
    auto* sp = static_cast<SrtPusher*>(data);

    if (!sp->active) return;
    sp->active = false;

    obs_output_end_data_capture(sp->output);

    if (sp->fmt_ctx) {
        av_write_trailer(sp->fmt_ctx);
        if (sp->fmt_ctx->pb)
            avio_closep(&sp->fmt_ctx->pb);
        avformat_free_context(sp->fmt_ctx);
        sp->fmt_ctx = nullptr;
    }

    MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher: stopped");
}

static void srt_pusher_encoded_packet(void* data, struct encoder_packet* packet) {
    auto* sp = static_cast<SrtPusher*>(data);

    if (!sp->active || !sp->fmt_ctx) return;

    int stream_idx = (packet->type == OBS_ENCODER_VIDEO)
        ? sp->video_stream_idx : sp->audio_stream_idx;
    if (stream_idx < 0) return;

    AVStream* stream = sp->fmt_ctx->streams[stream_idx];
    if (!stream) return;

    // ── Build AVPacket ────────────────────────────────────────
    // OBS's H.264 (QSV) packets begin with "00 00 00 00 01" — a valid 4-byte
    // Annex-B start code with one superfluous leading zero. That extra zero
    // makes the mpegts muxer reject the first (keyframe) packet with
    // "Invalid data found when processing input", so the stream loses its first
    // keyframe and the player stalls until the next GOP (most visible on the
    // low-bitrate 360p rendition). Drop leading zero bytes so the payload starts
    // on a clean "00 00 00 01" start code. HEVC already starts clean (no-op).
    const uint8_t* pdata = packet->data;
    size_t         psize = packet->size;
    if (packet->type == OBS_ENCODER_VIDEO) {
        while (psize >= 5 &&
               pdata[0] == 0 && pdata[1] == 0 && pdata[2] == 0 &&
               pdata[3] == 0 && pdata[4] == 1) {
            ++pdata;
            --psize;
        }
    }

    AVPacket avpkt{};
    avpkt.pts          = AV_NOPTS_VALUE;
    avpkt.dts          = AV_NOPTS_VALUE;
    avpkt.pos          = -1;
    avpkt.data       = const_cast<uint8_t*>(pdata);
    avpkt.size       = (int)psize;
    avpkt.stream_index = stream_idx;
    avpkt.flags      = 0;

    if (packet->keyframe)
        avpkt.flags |= AV_PKT_FLAG_KEY;
    if (packet->priority == 0)
        avpkt.flags |= AV_PKT_FLAG_KEY;

    // Rescale PTS/DTS from the packet's OWN timebase into the stream timebase.
    // OBS video packets use {1, fps} and audio {1, sample_rate} — NOT 90kHz.
    // The previous code hardcoded {1,90000}, so every timestamp came out
    // ~3000x too small → non-monotonic / invalid TS that the ingest rejected.
    AVRational enc_tb = { packet->timebase_num, packet->timebase_den };
    if (enc_tb.num <= 0 || enc_tb.den <= 0)
        enc_tb = AVRational{1, 90000};

    int64_t in_dts = (packet->dts != AV_NOPTS_VALUE) ? packet->dts : packet->pts;
    int64_t pts = rescale_ts(packet->pts, enc_tb, stream->time_base);
    int64_t dts = rescale_ts(in_dts,      enc_tb, stream->time_base);

    // Shared zero-epoch for audio+video (both stream time_bases are 90kHz) so
    // the two tracks stay in sync regardless of which packet arrives first.
    if (sp->start_ts < 0)
        sp->start_ts = dts;

    avpkt.pts = pts - sp->start_ts;
    avpkt.dts = dts - sp->start_ts;

    // DIAG: first packet of each type — reveals Annex-B vs AVCC payload + timing
    if (packet->type == OBS_ENCODER_VIDEO && !sp->logged_v) {
        sp->logged_v = true;
        const uint8_t* d = packet->data;
        MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher[%s]: first VIDEO pkt bytes=%02x %02x %02x %02x %02x size=%zu key=%d tb=%d/%d pts=%lld dts=%lld",
               sp->name.c_str(), d[0], d[1], d[2], d[3], d[4], packet->size, packet->keyframe ? 1 : 0,
               packet->timebase_num, packet->timebase_den,
               (long long)avpkt.pts, (long long)avpkt.dts);
    } else if (packet->type != OBS_ENCODER_VIDEO && !sp->logged_a) {
        sp->logged_a = true;
        MP_LOG(LOG_INFO, "[obs-multipusher] srt_pusher[%s]: first AUDIO pkt size=%zu tb=%d/%d pts=%lld",
               sp->name.c_str(), packet->size, packet->timebase_num, packet->timebase_den,
               (long long)avpkt.pts);
    }

    // ── Write ──────────────────────────────────────────────────
    int ret = av_interleaved_write_frame(sp->fmt_ctx, &avpkt);
    if (ret < 0) {
        // Per-output error log (first 8) so we can tell which stream fails and how.
        if (sp->werr < 8) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            MP_LOG(LOG_WARNING, "[obs-multipusher] srt_pusher[%s]: write error (%s pkt): %s",
                   sp->name.c_str(), packet->type == OBS_ENCODER_VIDEO ? "video" : "audio", errbuf);
            sp->werr++;
        }
    }
}

// ── Public registration ───────────────────────────────────────────

void RegisterSRTPusherOutput() {
    // Surface FFmpeg / libsrt diagnostics in the OBS log.
    av_log_set_callback(srt_pusher_av_log);

    obs_output_info info = {};
    info.id             = "mp_srt_pusher";
    info.flags          = OBS_OUTPUT_ENCODED | OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO;
    info.get_name       = srt_pusher_get_name;
    info.create         = srt_pusher_create;
    info.destroy        = srt_pusher_destroy;
    info.start          = srt_pusher_start;
    info.stop           = srt_pusher_stop;
    info.encoded_packet = srt_pusher_encoded_packet;

    obs_register_output(&info);
    MP_LOG(LOG_INFO, "[obs-multipusher] registered custom output type: mp_srt_pusher");
}
