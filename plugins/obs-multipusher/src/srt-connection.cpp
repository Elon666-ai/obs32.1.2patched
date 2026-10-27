#include "srt-connection.hpp"
#include "utils.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <srt/srt.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool findQueryParam(const std::string &url, const char *key, std::string &out)
{
    const auto q = url.find('?');
    if (q == std::string::npos)
        return false;

    const std::string k(key);
    size_t pos = q + 1;
    while (pos <= url.size()) {
        size_t amp = url.find('&', pos);
        const size_t len = (amp == std::string::npos) ? std::string::npos : amp - pos;
        std::string pair = url.substr(pos, len);
        if (pair.size() > k.size() && pair.compare(0, k.size(), k) == 0 &&
            pair[k.size()] == '=') {
            out = pair.substr(k.size() + 1);
            return true;
        }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return false;
}

} // namespace

SrtConnection::~SrtConnection()
{
    Close();
}

bool SrtConnection::applyUrlOptions(const std::string &url, std::string &err)
{
    payloadSize_ = 1316;
    latencyMs_ = -1;
    hasStreamId_ = false;
    streamId_.clear();

    std::string v;
    if (findQueryParam(url, "pkt_size", v) ||
        findQueryParam(url, "payload_size", v)) {
        const int n = std::atoi(v.c_str());
        if (n > 0 && n <= SRT_LIVE_MAX_PLSIZE)
            payloadSize_ = n;
    }

    if (findQueryParam(url, "latency", v) ||
        findQueryParam(url, "tsbpddelay", v)) {
        const long long us = std::strtoll(v.c_str(), nullptr, 10);
        if (us > 0)
            latencyMs_ = static_cast<int>(us / 1000);
    }

    if (findQueryParam(url, "streamid", v) ||
        findQueryParam(url, "srt_streamid", v)) {
        streamId_ = v;
        hasStreamId_ = !v.empty();
    }

    err.clear();
    return true;
}

bool SrtConnection::Open(const std::string &url, const std::string &name,
                         std::string &err)
{
    Close();

    name_ = name;
    if (!applyUrlOptions(url, err))
        return false;

    // srt_startup() is reference-counted by libsrt; balance it in Close() so
    // several pusher outputs (and OBS's own SRT output) can coexist safely.
    if (srt_startup() < 0) {
        err = std::string("srt_startup: ") + srt_getlasterror_str();
        return false;
    }
    srtStarted_ = true;

    bool success = false;
    struct StartupGuard {
        bool &started;
        bool &ok;
        ~StartupGuard()
        {
            if (!ok && started) {
                srt_cleanup();
                started = false;
            }
        }
    } guard{srtStarted_, success};

    char proto[16] = {0};
    char host[256] = {0};
    char path[1024] = {0};
    int port = 0;
    av_url_split(proto, sizeof(proto), nullptr, 0, host, sizeof(host), &port,
                 path, sizeof(path), url.c_str());

    if (std::strcmp(proto, "srt") != 0) {
        err = "URL is not an srt:// URL";
        return false;
    }
    if (port <= 0 || port > 65535) {
        err = "missing or invalid SRT port";
        return false;
    }
    if (!host[0]) {
        err = "missing SRT host";
        return false;
    }

    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *ai = nullptr;
    const int gai = getaddrinfo(host, portStr, &hints, &ai);
    if (gai != 0) {
        err = std::string("resolve ") + host + " failed (getaddrinfo error " +
              std::to_string(gai) + ")";
        return false;
    }

    std::string lastErr;
    int connectTimeoutMs = 5000;

    for (struct addrinfo *cur = ai; cur; cur = cur->ai_next) {
        SRTSOCKET fd = srt_create_socket();
        if (fd == SRT_INVALID_SOCK) {
            lastErr = std::string("create socket: ") + srt_getlasterror_str();
            continue;
        }

        bool ok = true;
        auto setOpt = [&](SRT_SOCKOPT opt, const char *label, const void *val,
                          int len) {
            if (srt_setsockopt(fd, 0, opt, val, len) < 0) {
                lastErr = std::string(label) + ": " + srt_getlasterror_str();
                ok = false;
                return false;
            }
            return true;
        };

        SRT_TRANSTYPE transtype = SRTT_LIVE;
        setOpt(SRTO_TRANSTYPE, "SRTO_TRANSTYPE", &transtype,
               sizeof(transtype));

        const int yes = 1;
        ok = ok && setOpt(SRTO_SENDER, "SRTO_SENDER", &yes, sizeof(yes));
        ok = ok && setOpt(SRTO_TSBPDMODE, "SRTO_TSBPDMODE", &yes, sizeof(yes));
        ok = ok && setOpt(SRTO_TLPKTDROP, "SRTO_TLPKTDROP", &yes, sizeof(yes));

        // Blocking for connect so a bad host/port fails fast with a reason.
        int blocking = 1;
        ok = ok && setOpt(SRTO_SNDSYN, "SRTO_SNDSYN", &blocking,
                          sizeof(blocking));
        ok = ok && setOpt(SRTO_CONNTIMEO, "SRTO_CONNTIMEO", &connectTimeoutMs,
                          sizeof(connectTimeoutMs));

        if (hasStreamId_)
            ok = ok && setOpt(SRTO_STREAMID, "SRTO_STREAMID",
                              streamId_.data(),
                              static_cast<int>(streamId_.size()));
        if (latencyMs_ >= 0)
            ok = ok && setOpt(SRTO_LATENCY, "SRTO_LATENCY", &latencyMs_,
                              sizeof(latencyMs_));
        if (payloadSize_ > 0)
            ok = ok && setOpt(SRTO_PAYLOADSIZE, "SRTO_PAYLOADSIZE",
                              &payloadSize_, sizeof(payloadSize_));

        if (!ok) {
            srt_close(fd);
            continue;
        }

        if (srt_connect(fd, cur->ai_addr,
                        static_cast<int>(cur->ai_addrlen)) < 0) {
            const int rej = srt_getrejectreason(fd);
            lastErr = std::string("connect: ") + srt_getlasterror_str();
            if (rej > SRT_REJ_UNKNOWN)
                lastErr += std::string(" (") + srt_rejectreason_str(rej) + ")";
            srt_close(fd);
            continue;
        }

        if (srt_getsockstate(fd) != SRTS_CONNECTED) {
            lastErr = "socket not connected after srt_connect";
            srt_close(fd);
            continue;
        }

        // Non-blocking I/O: Write() waits on the SRT epoll with a timeout
        // instead of blocking the output thread forever on a congested link.
        const int nonblocking = 0;
        if (srt_setsockopt(fd, 0, SRTO_SNDSYN, &nonblocking,
                           sizeof(nonblocking)) < 0 ||
            srt_setsockopt(fd, 0, SRTO_RCVSYN, &nonblocking,
                           sizeof(nonblocking)) < 0) {
            lastErr = std::string("non-blocking mode: ") +
                      srt_getlasterror_str();
            srt_close(fd);
            continue;
        }

        const int eid = srt_epoll_create();
        if (eid < 0) {
            lastErr = std::string("epoll create: ") + srt_getlasterror_str();
            srt_close(fd);
            continue;
        }

        int modes = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        if (srt_epoll_add_usock(eid, fd, &modes) < 0) {
            lastErr = std::string("epoll add: ") + srt_getlasterror_str();
            srt_epoll_release(eid);
            srt_close(fd);
            continue;
        }

        fd_ = fd;
        eid_ = eid;
        openedMs_ = nowMs();
        lastStatsMs_ = 0;
        lastLogMs_ = openedMs_;
        break;
    }

    freeaddrinfo(ai);

    if (fd_ < 0) {
        err = lastErr.empty() ? "unknown SRT connect failure" : lastErr;
        return false;
    }

    maybeUpdateStats(false);

    success = true;

    MP_LOG(LOG_INFO,
           "[obs-multipusher] srt[%s]: connected payload=%d latency=%dms "
           "streamid=%s",
           name_.c_str(), payloadSize_, latencyMs_,
           hasStreamId_ ? "yes" : "no");
    return true;
}

void SrtConnection::Close()
{
    if (fd_ >= 0) {
        maybeUpdateStats(true);

        SrtStats st;
        if (GetStats(st) && st.valid) {
            MP_LOG(LOG_INFO,
                   "[obs-multipusher] srt[%s] session summary: "
                   "sent=%.1fMB packet_loss=%.2f%% (%lld/%lld) "
                   "unrecoverable_drop=%.2f%% retrans=%.2f%% rtt=%.1fms "
                   "duration=%.1fs",
                   name_.c_str(),
                   static_cast<double>(st.bytesSent) / 1000000.0,
                   st.lossPercent, static_cast<long long>(st.packetsLost),
                   static_cast<long long>(st.packetsSent + st.packetsLost),
                   st.dropPercent, st.retransPercent, st.rttMs,
                   static_cast<double>(st.durationMs) / 1000.0);
        }

        if (eid_ >= 0) {
            srt_epoll_release(eid_);
            eid_ = -1;
        }
        srt_close(fd_);
        fd_ = -1;
    }

    if (srtStarted_) {
        srt_cleanup();
        srtStarted_ = false;
    }

    {
        std::lock_guard<std::mutex> lk(statsMu_);
        stats_ = SrtStats{};
    }
}

int SrtConnection::Write(const uint8_t *buf, int size)
{
    if (fd_ < 0 || !buf || size <= 0)
        return AVERROR(EIO);

    int sent = 0;
    const int64_t deadline = nowMs() + 3000;

    while (sent < size) {
        const int n = srt_send(fd_,
                               reinterpret_cast<const char *>(buf) + sent,
                               size - sent);
        if (n > 0) {
            sent += n;
            continue;
        }

        const int err = srt_getlasterror(nullptr);
        if (err == SRT_EASYNCSND) {
            const int64_t remaining = deadline - nowMs();
            if (remaining <= 0) {
                MP_LOG(LOG_WARNING,
                       "[obs-multipusher] srt[%s]: send timed out "
                       "(congested link)",
                       name_.c_str());
                maybeUpdateStats(true);
                return AVERROR(ETIMEDOUT);
            }

            const int64_t waitMs = std::min<int64_t>(remaining, 100);
            SRTSOCKET errFds[1] = {SRT_INVALID_SOCK};
            int errNum = 1;
            SRTSOCKET writeFds[1] = {SRT_INVALID_SOCK};
            int writeNum = 1;
            const int r =
                srt_epoll_wait(eid_, errFds, &errNum, writeFds, &writeNum,
                               waitMs, nullptr, nullptr, nullptr, nullptr);
            if (r < 0) {
                const int e2 = srt_getlasterror(nullptr);
                if (e2 == SRT_ETIMEOUT)
                    continue;
                MP_LOG(LOG_WARNING,
                       "[obs-multipusher] srt[%s]: epoll wait failed: %s",
                       name_.c_str(), srt_getlasterror_str());
                maybeUpdateStats(true);
                return AVERROR(EIO);
            }
            if (errNum > 0 && errFds[0] != SRT_INVALID_SOCK) {
                MP_LOG(LOG_WARNING,
                       "[obs-multipusher] srt[%s]: socket reported an error",
                       name_.c_str());
                maybeUpdateStats(true);
                return AVERROR(EIO);
            }
            continue;
        }

        MP_LOG(LOG_WARNING, "[obs-multipusher] srt[%s]: send failed: %s",
               name_.c_str(), srt_getlasterror_str());
        maybeUpdateStats(true);
        return AVERROR(EIO);
    }

    maybeUpdateStats(false);
    return size;
}

bool SrtConnection::GetStats(SrtStats &out) const
{
    std::lock_guard<std::mutex> lk(statsMu_);
    out = stats_;
    return out.valid;
}

void SrtConnection::RefreshStats()
{
    maybeUpdateStats(true);
}

void SrtConnection::maybeUpdateStats(bool force)
{
    if (fd_ < 0)
        return;

    const int64_t now = nowMs();
    if (!force && now - lastStatsMs_ < 1000)
        return;
    lastStatsMs_ = now;

    SRT_TRACEBSTATS perf = {};
    if (srt_bstats(fd_, &perf, 0) != 0)
        return;

    SrtStats st;
    st.valid = true;
    st.packetsSent = perf.pktSentTotal;
    st.packetsLost = perf.pktSndLossTotal;
    st.packetsDropped = perf.pktSndDropTotal;
    st.packetsRetrans = perf.pktRetransTotal;
    st.bytesSent = static_cast<int64_t>(perf.byteSentTotal);
    st.bytesDropped = static_cast<int64_t>(perf.byteSndDropTotal);

    const int64_t attempted = st.packetsSent + st.packetsLost;
    st.lossPercent = attempted > 0
                         ? 100.0 * static_cast<double>(st.packetsLost) /
                               static_cast<double>(attempted)
                         : 0.0;

    const int64_t dropAttempted = st.packetsSent + st.packetsDropped;
    st.dropPercent = dropAttempted > 0
                         ? 100.0 * static_cast<double>(st.packetsDropped) /
                               static_cast<double>(dropAttempted)
                         : 0.0;

    st.retransPercent =
        st.packetsSent > 0
            ? 100.0 * static_cast<double>(st.packetsRetrans) /
                  static_cast<double>(st.packetsSent)
            : 0.0;

    st.bytesLost = static_cast<int64_t>(static_cast<double>(st.bytesSent) *
                                        st.lossPercent / 100.0);
    st.rttMs = perf.msRTT;
    st.sendRateMbps = perf.mbpsSendRate;
    st.durationMs = perf.msTimeStamp;

    {
        std::lock_guard<std::mutex> lk(statsMu_);
        stats_ = st;
    }

    if (now - lastLogMs_ >= 10000) {
        lastLogMs_ = now;
        MP_LOG(LOG_INFO,
               "[obs-multipusher] srt[%s] stats: "
               "packet_loss=%.2f%% (%lld/%lld) unrecoverable_drop=%.2f%% "
               "retrans=%.2f%% rtt=%.1fms rate=%.2fMbps sent=%.1fMB",
               name_.c_str(), st.lossPercent,
               static_cast<long long>(st.packetsLost),
               static_cast<long long>(st.packetsSent + st.packetsLost),
               st.dropPercent, st.retransPercent, st.rttMs, st.sendRateMbps,
               static_cast<double>(st.bytesSent) / 1000000.0);
    }
}
