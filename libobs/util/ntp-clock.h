/*
 * Absolute wall-clock time source, disciplined against public NTP
 * servers at startup and periodically thereafter.
 *
 * ntp_clock_now_ms() returns milliseconds since the Unix epoch (UTC),
 * corrected by the most recent NTP offset measurement. If no NTP
 * server has ever been reachable, it falls back to the local system
 * wall clock so callers always get a usable (if uncorrected) value.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "c99defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the background NTP sync thread. Safe to call once during
 * obs_startup(); non-blocking (the first sync happens asynchronously). */
EXPORT void ntp_clock_init(void);

/* Stops the background NTP sync thread. Safe to call during
 * obs_shutdown(); safe to call even if ntp_clock_init() was never
 * called. */
EXPORT void ntp_clock_free(void);

/* Current absolute time, milliseconds since the Unix epoch (UTC). */
EXPORT uint64_t ntp_clock_now_ms(void);

/* Whether at least one NTP sync has ever succeeded. */
EXPORT bool ntp_clock_is_synced(void);

#ifdef __cplusplus
}
#endif
