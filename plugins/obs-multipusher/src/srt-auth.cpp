#include "srt-auth.hpp"

#include <ctime>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

// ── MD5 implementation (RFC 1321) ─────────────────────────────────

namespace {

// MD5 context
struct MD5_CTX {
    uint32_t state[4];       // A, B, C, D
    uint32_t count[2];       // bits count
    uint8_t  buffer[64];     // input buffer
};

// Constants for MD5Transform
constexpr uint32_t S11 = 7;
constexpr uint32_t S12 = 12;
constexpr uint32_t S13 = 17;
constexpr uint32_t S14 = 22;
constexpr uint32_t S21 = 5;
constexpr uint32_t S22 = 9;
constexpr uint32_t S23 = 14;
constexpr uint32_t S24 = 20;
constexpr uint32_t S31 = 4;
constexpr uint32_t S32 = 11;
constexpr uint32_t S33 = 16;
constexpr uint32_t S34 = 23;
constexpr uint32_t S41 = 6;
constexpr uint32_t S42 = 10;
constexpr uint32_t S43 = 15;
constexpr uint32_t S44 = 21;

inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
inline uint32_t ROTATE_LEFT(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

inline void FF(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a += F(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}
inline void GG(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a += G(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}
inline void HH(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a += H(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}
inline void II(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac) {
    a += I(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}

void MD5Init(MD5_CTX* ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

void MD5Update(MD5_CTX* ctx, const uint8_t* input, size_t inputLen) {
    size_t i = 0;
    size_t index = (ctx->count[0] >> 3) & 0x3F;
    size_t partLen = 64 - index;

    ctx->count[0] += static_cast<uint32_t>(inputLen << 3);
    if (ctx->count[0] < static_cast<uint32_t>(inputLen << 3))
        ctx->count[1]++;
    ctx->count[1] += static_cast<uint32_t>(inputLen >> 29);

    if (inputLen >= partLen) {
        memcpy(&ctx->buffer[index], input, partLen);
        // MD5Transform inlined
        uint32_t x[16];
        uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
        for (int j = 0; j < 16; ++j)
            x[j] = reinterpret_cast<const uint32_t*>(ctx->buffer)[j];

        FF (a, b, c, d, x[ 0], S11, 0xd76aa478); FF (d, a, b, c, x[ 1], S12, 0xe8c7b756);
        FF (c, d, a, b, x[ 2], S13, 0x242070db); FF (b, c, d, a, x[ 3], S14, 0xc1bdceee);
        FF (a, b, c, d, x[ 4], S11, 0xf57c0faf); FF (d, a, b, c, x[ 5], S12, 0x4787c62a);
        FF (c, d, a, b, x[ 6], S13, 0xa8304613); FF (b, c, d, a, x[ 7], S14, 0xfd469501);
        FF (a, b, c, d, x[ 8], S11, 0x698098d8); FF (d, a, b, c, x[ 9], S12, 0x8b44f7af);
        FF (c, d, a, b, x[10], S13, 0xffff5bb1); FF (b, c, d, a, x[11], S14, 0x895cd7be);
        FF (a, b, c, d, x[12], S11, 0x6b901122); FF (d, a, b, c, x[13], S12, 0xfd987193);
        FF (c, d, a, b, x[14], S13, 0xa679438e); FF (b, c, d, a, x[15], S14, 0x49b40821);

        GG (a, b, c, d, x[ 1], S21, 0xf61e2562); GG (d, a, b, c, x[ 6], S22, 0xc040b340);
        GG (c, d, a, b, x[11], S23, 0x265e5a51); GG (b, c, d, a, x[ 0], S24, 0xe9b6c7aa);
        GG (a, b, c, d, x[ 5], S21, 0xd62f105d); GG (d, a, b, c, x[10], S22,  0x2441453);
        GG (c, d, a, b, x[15], S23, 0xd8a1e681); GG (b, c, d, a, x[ 4], S24, 0xe7d3fbc8);
        GG (a, b, c, d, x[ 9], S21, 0x21e1cde6); GG (d, a, b, c, x[14], S22, 0xc33707d6);
        GG (c, d, a, b, x[ 3], S23, 0xf4d50d87); GG (b, c, d, a, x[ 8], S24, 0x455a14ed);
        GG (a, b, c, d, x[13], S21, 0xa9e3e905); GG (d, a, b, c, x[ 2], S22, 0xfcefa3f8);
        GG (c, d, a, b, x[ 7], S23, 0x676f02d9); GG (b, c, d, a, x[12], S24, 0x8d2a4c8a);

        HH (a, b, c, d, x[ 5], S31, 0xfffa3942); HH (d, a, b, c, x[ 8], S32, 0x8771f681);
        HH (c, d, a, b, x[11], S33, 0x6d9d6122); HH (b, c, d, a, x[14], S34, 0xfde5380c);
        HH (a, b, c, d, x[ 1], S31, 0xa4beea44); HH (d, a, b, c, x[ 4], S32, 0x4bdecfa9);
        HH (c, d, a, b, x[ 7], S33, 0xf6bb4b60); HH (b, c, d, a, x[10], S34, 0xbebfbc70);
        HH (a, b, c, d, x[13], S31, 0x289b7ec6); HH (d, a, b, c, x[ 0], S32, 0xeaa127fa);
        HH (c, d, a, b, x[ 3], S33, 0xd4ef3085); HH (b, c, d, a, x[ 6], S34,  0x4881d05);
        HH (a, b, c, d, x[ 9], S31, 0xd9d4d039); HH (d, a, b, c, x[12], S32, 0xe6db99e5);
        HH (c, d, a, b, x[15], S33, 0x1fa27cf8); HH (b, c, d, a, x[ 2], S34, 0xc4ac5665);

        II (a, b, c, d, x[ 0], S41, 0xf4292244); II (d, a, b, c, x[ 7], S42, 0x432aff97);
        II (c, d, a, b, x[14], S43, 0xab9423a7); II (b, c, d, a, x[ 5], S44, 0xfc93a039);
        II (a, b, c, d, x[12], S41, 0x655b59c3); II (d, a, b, c, x[ 3], S42, 0x8f0ccc92);
        II (c, d, a, b, x[10], S43, 0xffeff47d); II (b, c, d, a, x[ 1], S44, 0x85845dd1);
        II (a, b, c, d, x[ 8], S41, 0x6fa87e4f); II (d, a, b, c, x[15], S42, 0xfe2ce6e0);
        II (c, d, a, b, x[ 6], S43, 0xa3014314); II (b, c, d, a, x[13], S44, 0x4e0811a1);
        II (a, b, c, d, x[ 4], S41, 0xf7537e82); II (d, a, b, c, x[11], S42, 0xbd3af235);
        II (c, d, a, b, x[ 2], S43, 0x2ad7d2bb); II (b, c, d, a, x[ 9], S44, 0xeb86d391);

        ctx->state[0] += a;
        ctx->state[1] += b;
        ctx->state[2] += c;
        ctx->state[3] += d;

        // A full block was just consumed from the front of `input`; the buffer
        // is now empty, so any remainder must be staged at buffer[0]. The old
        // code left `index` at its original value AND advanced input/inputLen,
        // which made the trailing memcpy below write past ctx->buffer (a heap
        // overflow that corrupted memory for inputs of certain lengths, e.g.
        // a 63-byte hash for "gsp2w-fwv_standard_hevc").
        i = partLen;
        index = 0;
    }

    while (i + 64 <= inputLen) {
        uint32_t x[16];
        memcpy(x, input + i, 64);
        uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];

        FF (a, b, c, d, x[ 0], S11, 0xd76aa478); FF (d, a, b, c, x[ 1], S12, 0xe8c7b756);
        FF (c, d, a, b, x[ 2], S13, 0x242070db); FF (b, c, d, a, x[ 3], S14, 0xc1bdceee);
        FF (a, b, c, d, x[ 4], S11, 0xf57c0faf); FF (d, a, b, c, x[ 5], S12, 0x4787c62a);
        FF (c, d, a, b, x[ 6], S13, 0xa8304613); FF (b, c, d, a, x[ 7], S14, 0xfd469501);
        FF (a, b, c, d, x[ 8], S11, 0x698098d8); FF (d, a, b, c, x[ 9], S12, 0x8b44f7af);
        FF (c, d, a, b, x[10], S13, 0xffff5bb1); FF (b, c, d, a, x[11], S14, 0x895cd7be);
        FF (a, b, c, d, x[12], S11, 0x6b901122); FF (d, a, b, c, x[13], S12, 0xfd987193);
        FF (c, d, a, b, x[14], S13, 0xa679438e); FF (b, c, d, a, x[15], S14, 0x49b40821);

        GG (a, b, c, d, x[ 1], S21, 0xf61e2562); GG (d, a, b, c, x[ 6], S22, 0xc040b340);
        GG (c, d, a, b, x[11], S23, 0x265e5a51); GG (b, c, d, a, x[ 0], S24, 0xe9b6c7aa);
        GG (a, b, c, d, x[ 5], S21, 0xd62f105d); GG (d, a, b, c, x[10], S22,  0x2441453);
        GG (c, d, a, b, x[15], S23, 0xd8a1e681); GG (b, c, d, a, x[ 4], S24, 0xe7d3fbc8);
        GG (a, b, c, d, x[ 9], S21, 0x21e1cde6); GG (d, a, b, c, x[14], S22, 0xc33707d6);
        GG (c, d, a, b, x[ 3], S23, 0xf4d50d87); GG (b, c, d, a, x[ 8], S24, 0x455a14ed);
        GG (a, b, c, d, x[13], S21, 0xa9e3e905); GG (d, a, b, c, x[ 2], S22, 0xfcefa3f8);
        GG (c, d, a, b, x[ 7], S23, 0x676f02d9); GG (b, c, d, a, x[12], S24, 0x8d2a4c8a);

        HH (a, b, c, d, x[ 5], S31, 0xfffa3942); HH (d, a, b, c, x[ 8], S32, 0x8771f681);
        HH (c, d, a, b, x[11], S33, 0x6d9d6122); HH (b, c, d, a, x[14], S34, 0xfde5380c);
        HH (a, b, c, d, x[ 1], S31, 0xa4beea44); HH (d, a, b, c, x[ 4], S32, 0x4bdecfa9);
        HH (c, d, a, b, x[ 7], S33, 0xf6bb4b60); HH (b, c, d, a, x[10], S34, 0xbebfbc70);
        HH (a, b, c, d, x[13], S31, 0x289b7ec6); HH (d, a, b, c, x[ 0], S32, 0xeaa127fa);
        HH (c, d, a, b, x[ 3], S33, 0xd4ef3085); HH (b, c, d, a, x[ 6], S34,  0x4881d05);
        HH (a, b, c, d, x[ 9], S31, 0xd9d4d039); HH (d, a, b, c, x[12], S32, 0xe6db99e5);
        HH (c, d, a, b, x[15], S33, 0x1fa27cf8); HH (b, c, d, a, x[ 2], S34, 0xc4ac5665);

        II (a, b, c, d, x[ 0], S41, 0xf4292244); II (d, a, b, c, x[ 7], S42, 0x432aff97);
        II (c, d, a, b, x[14], S43, 0xab9423a7); II (b, c, d, a, x[ 5], S44, 0xfc93a039);
        II (a, b, c, d, x[12], S41, 0x655b59c3); II (d, a, b, c, x[ 3], S42, 0x8f0ccc92);
        II (c, d, a, b, x[10], S43, 0xffeff47d); II (b, c, d, a, x[ 1], S44, 0x85845dd1);
        II (a, b, c, d, x[ 8], S41, 0x6fa87e4f); II (d, a, b, c, x[15], S42, 0xfe2ce6e0);
        II (c, d, a, b, x[ 6], S43, 0xa3014314); II (b, c, d, a, x[13], S44, 0x4e0811a1);
        II (a, b, c, d, x[ 4], S41, 0xf7537e82); II (d, a, b, c, x[11], S42, 0xbd3af235);
        II (c, d, a, b, x[ 2], S43, 0x2ad7d2bb); II (b, c, d, a, x[ 9], S44, 0xeb86d391);

        ctx->state[0] += a;
        ctx->state[1] += b;
        ctx->state[2] += c;
        ctx->state[3] += d;

        i += 64;
    }

    if (i < inputLen)
        memcpy(&ctx->buffer[index], input + i, inputLen - i);
}

void MD5Final(uint8_t digest[16], MD5_CTX* ctx) {
    uint8_t padding[64] = { 0x80 };
    size_t index = (ctx->count[0] >> 3) & 0x3F;
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);

    uint32_t bits[2] = { ctx->count[0], ctx->count[1] };
    uint8_t  tail[8];
    memcpy(tail, bits, 8);

    MD5Update(ctx, padding, padLen);
    MD5Update(ctx, tail, 8);

    memcpy(digest, ctx->state, 16);
}

} // anonymous namespace

// ── Public API ─────────────────────────────────────────────────────

namespace SrtAuth {

std::string MD5Hex(const std::string& input) {
    MD5_CTX ctx;
    uint8_t digest[16];
    char hex[33];

    MD5Init(&ctx);
    MD5Update(&ctx, reinterpret_cast<const uint8_t*>(input.data()), input.size());
    MD5Final(digest, &ctx);

    for (int i = 0; i < 16; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[32] = '\0';
    return std::string(hex);
}

void GenTxSecret(const std::string& streamKey, int tokenDays,
                 std::string& txTime, std::string& txSecret) {
    if (tokenDays <= 0)
        tokenDays = TX_PUBLISH_TOKEN_DAYS;

    // Calculate expiry time: now + tokenDays * 86400 seconds
    time_t expiry = time(nullptr) + static_cast<time_t>(tokenDays) * 86400;

    // txTime: uppercase hex of unix timestamp
    char txTimeBuf[16];
    snprintf(txTimeBuf, sizeof(txTimeBuf), "%llX",
             static_cast<unsigned long long>(expiry));
    txTime = txTimeBuf;

    // txSecret: lowercase md5 of (TX_KEY_MAIN + streamKey + txTime)
    std::string hashInput = std::string(TX_KEY_MAIN) + streamKey + txTime;
    txSecret = MD5Hex(hashInput);
}

std::string BuildSRTURL(const std::string& host, int port,
                        const std::string& app, const std::string& streamKey,
                        int tokenDays) {
    // Trim leading/trailing '/' from app
    std::string trimmedApp = app;
    while (!trimmedApp.empty() && trimmedApp.front() == '/') trimmedApp.erase(0, 1);
    while (!trimmedApp.empty() && trimmedApp.back() == '/') trimmedApp.pop_back();

    std::string txTime, txSecret;
    GenTxSecret(streamKey, tokenDays, txTime, txSecret);

    // Build URL with SRT transport params matching working OBS profiles
    char url[1024];
    snprintf(url, sizeof(url),
             "srt://%s:%d?streamid=#!::h=%s,r=%s/%s,txSecret=%s,txTime=%s&latency=200000&pkt_size=1316",
             host.c_str(), port, host.c_str(), trimmedApp.c_str(),
             streamKey.c_str(), txSecret.c_str(), txTime.c_str());
    return std::string(url);
}

std::string ExtractStreamKey(const std::string& rValue) {
    auto pos = rValue.find('/');
    if (pos == std::string::npos)
        return rValue;
    return rValue.substr(pos + 1);
}

std::string ResolveStreamName(const std::string& siteName,
                               const std::string& templateStr) {
    std::string result = templateStr;
    std::string lowerResult = templateStr;
    std::transform(lowerResult.begin(), lowerResult.end(),
                   lowerResult.begin(), ::tolower);

    // Replace all known template patterns
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = lowerResult.find(from, pos)) != std::string::npos) {
            result.replace(pos, from.length(), to);
            lowerResult.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    replaceAll("{sitename}", siteName);
    replaceAll("{siteName}", siteName);
    replaceAll("${sitename}", siteName);
    replaceAll("${siteName}", siteName);
    replaceAll("{site}", siteName);
    replaceAll("${site}", siteName);

    return result;
}

} // namespace SrtAuth
