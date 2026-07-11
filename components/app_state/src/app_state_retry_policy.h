#pragma once

// Pure C, host-compilable: no ESP-IDF includes, no dynamic allocation.
// Owns the *decisions* app_state_sync_task acts on - error classification,
// backoff timing, gap-driven resync - the same policy/adapter split
// wifi_manager uses (see wifi_policy.h). market_data_client.h's
// market_data_err_t is pure C itself, so it's safe to depend on here.

#include <stdbool.h>
#include <stdint.h>

#include "market_data_client.h"

#ifdef __cplusplus
extern "C" {
#endif

// True if err is worth retrying with backoff (network/timeout/rate-limit/
// clock-not-synced-yet/transient parse or allocation failure). False for
// errors where retrying the identical request cannot succeed (bad
// arguments, a symbol Binance doesn't recognize) - those need the caller
// to change the input (e.g. edit the watchlist), not a retry.
bool app_state_retry_is_recoverable(market_data_err_t err);

// Consecutive MARKET_DATA_ERR_SYMBOL_NOT_FOUND responses tolerated before a
// symbol is treated as unsupported on the current server and retries stop
// for the rest of the session (see app_state_retry_invalid_symbol_is_recoverable).
#define APP_STATE_MAX_INVALID_SYMBOL_ATTEMPTS 3

// True while a symbol that just got its Nth consecutive "invalid symbol"
// response (prior_strikes = N-1, i.e. how many were recorded before this
// one) should still be retried (state stays DEGRADED / "Resyncing...").
// False once the session-long attempt budget is exhausted, at which point
// the caller should treat the failure as unrecoverable (state -> ERROR) so
// the symbol stops being retried until a region change resets its counter.
bool app_state_retry_invalid_symbol_is_recoverable(uint8_t prior_strikes);

// Exponential backoff with a cap: base_ms * 2^attempt, clamped to max_ms.
// attempt is the consecutive-failure count (0 yields base_ms, 1 yields
// 2*base_ms, and so on).
uint32_t app_state_retry_backoff_delay_ms(uint32_t base_ms, uint32_t max_ms, uint8_t attempt);

// True if a connectivity gap of disconnected_ms is long enough that the
// existing klines history may already have a hole in it (at least one full
// candle interval was missed while offline), and a full resync should run
// once reconnected rather than trusting the old data to still be current.
bool app_state_retry_needs_resync(uint32_t disconnected_ms, uint32_t interval_ms);

#ifdef __cplusplus
}
#endif
