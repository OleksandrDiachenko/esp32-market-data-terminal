#pragma once

// Pure C, host-compilable: no ESP-IDF deps - see app_state_retry_policy.h
// for the precedent this follows. Applies one live `@kline_1s` update onto
// the same oldest-first candle array app_state stores per symbol, following
// standard exchange candle-update rules: update the in-progress candle if
// the update still belongs to it, or roll over to a new one if it doesn't.

#include <stdbool.h>
#include <stdint.h>

#include "market_data_client.h"       // market_data_kline_t
#include "market_data_kline_update.h" // market_data_kline_update_t

#ifdef __cplusplus
extern "C"
{
#endif

    // candles is an oldest-first array of capacity entries, *count of them
    // currently populated. Applies one 1s kline update in place:
    //
    //  - *count == 0: nothing to merge into yet (REST hasn't bootstrapped this
    //    symbol) - untouched, returns false.
    //  - update's bucket (floor(update->open_time_ms / interval_ms) * interval_ms)
    //    equals candles[*count-1].open_time_ms: merges into the last candle -
    //    high/low widen, close tracks update->close on every call, but
    //    volume/number_of_trades only accumulate when update->is_final is true
    //    (Binance re-sends the same in-progress 1s kline repeatedly before it
    //    closes; accumulating on every call would double-count) - returns false.
    //  - the bucket is newer than the last candle: appends a new candle,
    //    evicting the oldest (index 0) if already at capacity - returns true.
    //  - the bucket is older than the last candle (stale/out-of-order): ignored
    //    - returns false. Real gaps remain the job of the existing REST
    //    gap-detection resync, not this function.
    bool app_state_kline_merge_apply(market_data_kline_t *candles, uint16_t *count, uint16_t capacity,
                                     int64_t interval_ms, const market_data_kline_update_t *update);

#ifdef __cplusplus
}
#endif
