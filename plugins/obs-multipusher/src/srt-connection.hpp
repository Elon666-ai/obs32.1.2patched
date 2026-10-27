#pragma once

#include <cstdint>
#include <string>
#include <mutex>

/// Snapshot of libsrt sender statistics.
///
/// Two loss figures are exposed because they mean different things:
///  - lossPercent:      pktSndLossTotal / (pktSentTotal + pktSndLossTotal).
///                      Packet loss the sender saw/retransmitted. This is the
///                      same value OBS reports via its get_srt_stats proc, so
///                      multipusher and OBS can be compared directly.
///  - dropPercent:      pktSndDropTotal / (pktSentTotal + pktSndDropTotal).
///                      Packets dropped because they were too late to send
///                      (beyond the SRT latency window). On the sender these
///                      are genuinely unrecoverable: they were never delivered
///                      and cannot be retransmitted.
struct SrtStats {
    bool    valid            = false;
    int64_t packetsSent      = 0;  // pktSentTotal (incl. retransmissions)
    int64_t packetsLost      = 0;  // pktSndLossTotal
    int64_t packetsDropped   = 0;  // pktSndDropTotal (too late to send)
    int64_t packetsRetrans   = 0;  // pktRetransTotal
    int64_t bytesSent        = 0;  // byteSentTotal
    int64_t bytesLost        = 0;  // sender-side lost bytes (derived)
    int64_t bytesDropped     = 0;  // byteSndDropTotal
    double  rttMs            = 0.0;
    double  sendRateMbps     = 0.0;
    double  lossPercent      = 0.0;  // OBS-compatible packet loss
    double  dropPercent      = 0.0;  // unrecoverable (too-late) drops
    double  retransPercent   = 0.0;  // retransmitted / sent
    int64_t durationMs       = 0;    // msTimeStamp
};

/// Minimal libsrt sender (caller mode) used by the mp_srt_pusher output.
///
/// FFmpeg's built-in srt:// protocol hides its socket, so its SRT statistics
/// cannot be read. This class owns the SRT socket directly (like OBS's own
/// mpegts output) and exposes SRT_TRACEBSTATS via GetStats(). The mpegts
/// muxer keeps using libavformat through a custom AVIOContext whose write
/// callback feeds SrtConnection::Write.
class SrtConnection {
public:
    SrtConnection() = default;
    ~SrtConnection();

    SrtConnection(const SrtConnection&) = delete;
    SrtConnection& operator=(const SrtConnection&) = delete;

    /// Parse the srt:// URL, connect, and configure the socket for live
    /// MPEG-TS sending. `name` is used only for log lines.
    bool Open(const std::string& url, const std::string& name, std::string& err);

    /// Close the socket and emit a final session summary.
    void Close();

    /// Write bytes; returns size on success or a negative AVERROR value.
    int Write(const uint8_t* buf, int size);

    /// AVIO buffer / SRT payload size parsed from the URL (default 1316).
    int PayloadSize() const { return payloadSize_; }

    /// Thread-safe copy of the most recent statistics snapshot.
    bool GetStats(SrtStats& out) const;

    /// Refresh the cached statistics immediately (output thread only).
    void RefreshStats();

    bool IsOpen() const { return fd_ >= 0; }

private:
    void maybeUpdateStats(bool force);
    bool applyUrlOptions(const std::string& url, std::string& err);

    int fd_  = -1;  // SRTSOCKET
    int eid_ = -1;  // SRT epoll id for non-blocking sends
    bool srtStarted_ = false;  // balances srt_startup()/srt_cleanup()

    std::string name_;
    int payloadSize_ = 1316;
    std::string streamId_;
    bool  hasStreamId_ = false;
    int   latencyMs_   = -1;

    int64_t lastStatsMs_ = 0;
    int64_t lastLogMs_   = 0;
    int64_t openedMs_    = 0;

    mutable std::mutex statsMu_;
    SrtStats stats_;
};
