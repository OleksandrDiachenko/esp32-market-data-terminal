#include "app_state_kline_merge.h"

#include <string.h>

static int64_t bucket_start_for(int64_t open_time_ms, int64_t interval_ms)
{
    return (open_time_ms / interval_ms) * interval_ms;
}

bool app_state_kline_merge_apply(market_data_kline_t *candles, uint16_t *count, uint16_t capacity, int64_t interval_ms,
                                 const market_data_kline_update_t *update)
{
    if (*count == 0)
    {
        return false; // REST hasn't bootstrapped this symbol yet - nothing to merge into
    }

    int64_t bucket_start = bucket_start_for(update->open_time_ms, interval_ms);
    market_data_kline_t *last = &candles[*count - 1];

    if (bucket_start == last->open_time_ms)
    {
        if (update->high > last->high)
        {
            last->high = update->high;
        }
        if (update->low < last->low)
        {
            last->low = update->low;
        }
        last->close = update->close;
        if (update->is_final)
        {
            last->volume += update->volume;
            last->number_of_trades += update->number_of_trades;
        }
        return false;
    }

    if (bucket_start < last->open_time_ms)
    {
        return false; // stale/out-of-order - REST resync remains the source of truth for real gaps
    }

    // bucket_start > last->open_time_ms: the next candle has started.
    market_data_kline_t new_candle;
    memset(&new_candle, 0, sizeof(new_candle));
    new_candle.open_time_ms = bucket_start;
    new_candle.open = update->open;
    new_candle.high = update->high;
    new_candle.low = update->low;
    new_candle.close = update->close;
    if (update->is_final)
    {
        new_candle.volume = update->volume;
        new_candle.number_of_trades = update->number_of_trades;
    }
    // close_time_ms/quote_asset_volume/taker_buy_* are left at zero - not
    // derivable from a single kline_1s update; the next REST resync fills
    // them in like every other field.

    if (*count == capacity)
    {
        memmove(&candles[0], &candles[1], (size_t)(capacity - 1) * sizeof(market_data_kline_t));
        candles[capacity - 1] = new_candle;
    }
    else
    {
        candles[*count] = new_candle;
        (*count)++;
    }
    return true;
}
