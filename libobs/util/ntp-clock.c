#include "ntp-clock.h"
#include "threading.h"
#include "platform.h"
#include "base.h"

#include <inttypes.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ntp_socket_t;
#define NTP_INVALID_SOCKET INVALID_SOCKET
#define ntp_close_socket closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
typedef int ntp_socket_t;
#define NTP_INVALID_SOCKET (-1)
#define ntp_close_socket close
#endif

/*
 * Minimal SNTP (RFC 4330-style) client. We deliberately skip the full
 * four-timestamp offset formula (which needs a client "originate"
 * timestamp already expressed in wall-clock terms) and instead assume
 * symmetric network delay: the current UTC time is estimated as the
 * server's Transmit Timestamp plus half the measured round-trip time.
 * This is the standard simplification used by minimal SNTP clients and
 * is accurate to within a few ms on typical low-jitter networks; it
 * degrades gracefully (larger error, never a hard failure) on bad
 * routes. We sample multiple servers/attempts and keep the lowest-RTT
 * result to minimize that error.
 */

#define NTP_PORT "123"
#define NTP_UNIX_EPOCH_DELTA_SEC 2208988800ULL /* 1900-01-01 -> 1970-01-01 */
#define NTP_PACKET_SIZE 48
#define NTP_SAMPLES_PER_SERVER 4
#define NTP_GOOD_ENOUGH_RTT_NS (30 * 1000000LL) /* stop early once RTT < 30ms */
#define NTP_RECV_TIMEOUT_MS 2000
#define NTP_RESYNC_INTERVAL_MS (30 * 60 * 1000) /* resync every 30 minutes */
#define NTP_RETRY_INTERVAL_MS (30 * 1000)       /* retry sooner if never synced */

static const char *ntp_servers[] = {
	"time.cloudflare.com",
	"time.windows.com",
	"pool.ntp.org",
};

struct ntp_anchor {
	pthread_mutex_t lock;
	int64_t offset_ms; /* ntp_utc_ms(t) - monotonic_ms(t), from the last successful sync */
	bool synced;
};

static struct ntp_anchor anchor;
static pthread_once_t ntp_once = PTHREAD_ONCE_INIT;

static pthread_t ntp_thread;
static os_event_t *ntp_stop_event = NULL;
static bool ntp_thread_running = false;

#ifdef _WIN32
static bool ntp_wsa_started = false;
#endif

static void ntp_lazy_init(void)
{
	pthread_mutex_init_value(&anchor.lock);
	pthread_mutex_init(&anchor.lock, NULL);
	anchor.offset_ms = 0;
	anchor.synced = false;
}

#ifdef _WIN32
static int64_t system_wall_clock_ms(void)
{
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);

	ULARGE_INTEGER uli;
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;

	/* FILETIME: 100-ns intervals since 1601-01-01 -> unix ms */
	const uint64_t epoch_diff_100ns = 116444736000000000ULL;
	return (int64_t)((uli.QuadPart - epoch_diff_100ns) / 10000ULL);
}
#else
static int64_t system_wall_clock_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* Queries one NTP server once. On success, *out_offset_ms is set such that
 * (monotonic_ms_now + *out_offset_ms) == current UTC ms, and *out_rtt_ns is
 * the measured round trip, used by the caller to pick the best sample. */
static bool ntp_query_once(const char *host, int64_t *out_offset_ms, int64_t *out_rtt_ns)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	bool ok = false;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	if (getaddrinfo(host, NTP_PORT, &hints, &res) != 0 || !res)
		return false;

	for (struct addrinfo *rp = res; rp != NULL && !ok; rp = rp->ai_next) {
		ntp_socket_t sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock == NTP_INVALID_SOCKET)
			continue;

#ifdef _WIN32
		DWORD timeout_ms = NTP_RECV_TIMEOUT_MS;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
		struct timeval tv;
		tv.tv_sec = NTP_RECV_TIMEOUT_MS / 1000;
		tv.tv_usec = (NTP_RECV_TIMEOUT_MS % 1000) * 1000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif

		if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) != 0) {
			ntp_close_socket(sock);
			continue;
		}

		uint8_t packet[NTP_PACKET_SIZE];
		memset(packet, 0, sizeof(packet));
		packet[0] = 0x23; /* LI = 0 (no warning), VN = 4, Mode = 3 (client) */

		int64_t mono_send_ns = (int64_t)os_gettime_ns();
		bool sent = send(sock, (const char *)packet, NTP_PACKET_SIZE, 0) == NTP_PACKET_SIZE;

		uint8_t reply[NTP_PACKET_SIZE];
		int received = sent ? recv(sock, (char *)reply, NTP_PACKET_SIZE, 0) : -1;
		int64_t mono_recv_ns = (int64_t)os_gettime_ns();

		ntp_close_socket(sock);

		if (received != NTP_PACKET_SIZE)
			continue;

		uint32_t tx_secs = ((uint32_t)reply[40] << 24) | ((uint32_t)reply[41] << 16) |
				   ((uint32_t)reply[42] << 8) | (uint32_t)reply[43];
		uint32_t tx_frac = ((uint32_t)reply[44] << 24) | ((uint32_t)reply[45] << 16) |
				   ((uint32_t)reply[46] << 8) | (uint32_t)reply[47];

		if (tx_secs <= (uint32_t)NTP_UNIX_EPOCH_DELTA_SEC)
			continue; /* garbage/empty reply */

		int64_t rtt_ns = mono_recv_ns - mono_send_ns;
		if (rtt_ns < 0)
			continue;

		uint64_t server_unix_ms =
			((uint64_t)tx_secs - NTP_UNIX_EPOCH_DELTA_SEC) * 1000ULL + (((uint64_t)tx_frac * 1000ULL) >> 32);

		int64_t utc_at_recv_ms = (int64_t)server_unix_ms + (rtt_ns / 2) / 1000000;
		int64_t mono_recv_ms = mono_recv_ns / 1000000;

		*out_offset_ms = utc_at_recv_ms - mono_recv_ms;
		*out_rtt_ns = rtt_ns;
		ok = true;
	}

	freeaddrinfo(res);
	return ok;
}

static void ntp_do_resync(void)
{
	int64_t best_offset_ms = 0;
	int64_t best_rtt_ns = INT64_MAX;
	bool got_any = false;

	for (size_t s = 0; s < sizeof(ntp_servers) / sizeof(ntp_servers[0]) && !got_any; s++) {
		for (int sample = 0; sample < NTP_SAMPLES_PER_SERVER; sample++) {
			int64_t offset_ms = 0, rtt_ns = 0;
			if (ntp_query_once(ntp_servers[s], &offset_ms, &rtt_ns) && rtt_ns < best_rtt_ns) {
				best_rtt_ns = rtt_ns;
				best_offset_ms = offset_ms;
				got_any = true;
			}
			if (got_any && best_rtt_ns < NTP_GOOD_ENOUGH_RTT_NS)
				break;
		}
	}

	if (!got_any) {
		blog(LOG_WARNING, "ntp-clock: all NTP servers unreachable, keeping previous offset (synced=%d)",
		     (int)ntp_clock_is_synced());
		return;
	}

	pthread_mutex_lock(&anchor.lock);
	anchor.offset_ms = best_offset_ms;
	anchor.synced = true;
	pthread_mutex_unlock(&anchor.lock);

	blog(LOG_INFO, "ntp-clock: synced, offset=%" PRId64 "ms rtt=%" PRId64 "ms", best_offset_ms,
	     best_rtt_ns / 1000000);
}

static void *ntp_thread_func(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("ntp-clock");

	for (;;) {
		ntp_do_resync();

		unsigned long wait_ms = ntp_clock_is_synced() ? NTP_RESYNC_INTERVAL_MS : NTP_RETRY_INTERVAL_MS;
		if (os_event_timedwait(ntp_stop_event, wait_ms) != ETIMEDOUT)
			break;
	}

	return NULL;
}

void ntp_clock_init(void)
{
	pthread_once(&ntp_once, ntp_lazy_init);

	if (ntp_thread_running)
		return;

#ifdef _WIN32
	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		blog(LOG_WARNING, "ntp-clock: WSAStartup failed, NTP sync disabled (falling back to system clock)");
		return;
	}
	ntp_wsa_started = true;
#endif

	if (os_event_init(&ntp_stop_event, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_WARNING, "ntp-clock: failed to create stop event, NTP sync disabled");
#ifdef _WIN32
		WSACleanup();
		ntp_wsa_started = false;
#endif
		return;
	}

	if (pthread_create(&ntp_thread, NULL, ntp_thread_func, NULL) != 0) {
		blog(LOG_WARNING, "ntp-clock: failed to start sync thread");
		os_event_destroy(ntp_stop_event);
		ntp_stop_event = NULL;
#ifdef _WIN32
		WSACleanup();
		ntp_wsa_started = false;
#endif
		return;
	}

	ntp_thread_running = true;
}

void ntp_clock_free(void)
{
	if (!ntp_thread_running)
		return;

	os_event_signal(ntp_stop_event);
	pthread_join(ntp_thread, NULL);
	os_event_destroy(ntp_stop_event);
	ntp_stop_event = NULL;
	ntp_thread_running = false;

#ifdef _WIN32
	if (ntp_wsa_started) {
		WSACleanup();
		ntp_wsa_started = false;
	}
#endif
}

uint64_t ntp_clock_now_ms(void)
{
	pthread_once(&ntp_once, ntp_lazy_init);

	pthread_mutex_lock(&anchor.lock);
	bool have_sync = anchor.synced;
	int64_t offset_ms = anchor.offset_ms;
	pthread_mutex_unlock(&anchor.lock);

	if (!have_sync)
		return (uint64_t)system_wall_clock_ms();

	int64_t mono_ms = (int64_t)(os_gettime_ns() / 1000000);
	return (uint64_t)(mono_ms + offset_ms);
}

bool ntp_clock_is_synced(void)
{
	pthread_once(&ntp_once, ntp_lazy_init);

	pthread_mutex_lock(&anchor.lock);
	bool s = anchor.synced;
	pthread_mutex_unlock(&anchor.lock);
	return s;
}
