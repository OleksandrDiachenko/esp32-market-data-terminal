#include "test_common.h"
#include "app_state_kline_merge.h"

#define INTERVAL_MS (5 * 60 * 1000)

static bool dbl_eq(double a, double b)
{
    double diff = a - b;
    if (diff < 0)
    {
        diff = -diff;
    }
    return diff < 1e-9;
}

static market_data_kline_t make_candle(int64_t open_time_ms, double open, double high, double low, double close,
                                       double volume, uint32_t trades)
{
    market_data_kline_t k;
    memset(&k, 0, sizeof(k));
    k.open_time_ms = open_time_ms;
    k.open = open;
    k.high = high;
    k.low = low;
    k.close = close;
    k.volume = volume;
    k.number_of_trades = trades;
    return k;
}

static market_data_kline_update_t make_update(int64_t open_time_ms, double open, double high, double low, double close,
                                              double volume, uint32_t trades, bool is_final)
{
    market_data_kline_update_t u;
    memset(&u, 0, sizeof(u));
    strcpy(u.symbol, "BTCUSDT");
    u.open_time_ms = open_time_ms;
    u.open = open;
    u.high = high;
    u.low = low;
    u.close = close;
    u.volume = volume;
    u.number_of_trades = trades;
    u.is_final = is_final;
    return u;
}

static void test_merge_non_final_widens_high_low_and_updates_close(void)
{
    market_data_kline_t candles[8];
    candles[0] = make_candle(0, 100, 105, 95, 102, 50, 10);
    uint16_t count = 1;

    market_data_kline_update_t u = make_update(1000, 102, 110, 90, 108, 999, 999, false);
    bool appended = app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &u);

    CHECK(appended == false);
    CHECK(count == 1);
    CHECK(dbl_eq(candles[0].high, 110));
    CHECK(dbl_eq(candles[0].low, 90));
    CHECK(dbl_eq(candles[0].close, 108));
    CHECK(dbl_eq(candles[0].volume, 50)); // unchanged - not final
    CHECK(candles[0].number_of_trades == 10);
}

static void test_merge_final_accumulates_volume_and_trades_exactly_once(void)
{
    market_data_kline_t candles[8];
    candles[0] = make_candle(0, 100, 105, 95, 102, 50, 10);
    uint16_t count = 1;

    market_data_kline_update_t u = make_update(1000, 102, 106, 96, 103, 5, 2, true);
    bool appended = app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &u);

    CHECK(appended == false);
    CHECK(dbl_eq(candles[0].volume, 55));
    CHECK(candles[0].number_of_trades == 12);
}

static void test_repeated_non_final_updates_freeze_volume_until_final(void)
{
    market_data_kline_t candles[8];
    candles[0] = make_candle(0, 100, 100, 100, 100, 0, 0);
    uint16_t count = 1;

    for (int i = 0; i < 5; i++)
    {
        market_data_kline_update_t u = make_update(1000, 100, 101, 99, 100 + i, 999, 999, false);
        app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &u);
    }
    CHECK(dbl_eq(candles[0].volume, 0));
    CHECK(candles[0].number_of_trades == 0);
    CHECK(dbl_eq(candles[0].close, 104)); // close still tracks the latest update

    market_data_kline_update_t final_u = make_update(1000, 100, 101, 99, 105, 12, 3, true);
    app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &final_u);
    CHECK(dbl_eq(candles[0].volume, 12));
    CHECK(candles[0].number_of_trades == 3);
}

static void test_append_evicts_oldest_at_capacity(void)
{
    market_data_kline_t candles[3];
    candles[0] = make_candle(0, 1, 1, 1, 1, 1, 1);
    candles[1] = make_candle(INTERVAL_MS, 2, 2, 2, 2, 2, 2);
    candles[2] = make_candle(2 * INTERVAL_MS, 3, 3, 3, 3, 3, 3);
    uint16_t count = 3;

    market_data_kline_update_t u = make_update(3 * INTERVAL_MS, 4, 4.5, 3.9, 4.2, 7, 9, false);
    bool appended = app_state_kline_merge_apply(candles, &count, 3, INTERVAL_MS, &u);

    CHECK(appended == true);
    CHECK(count == 3);
    CHECK(dbl_eq(candles[0].open, 2)); // oldest (index 0, open_time 0) evicted
    CHECK(dbl_eq(candles[1].open, 3));
    CHECK(candles[2].open_time_ms == 3 * INTERVAL_MS);
    CHECK(dbl_eq(candles[2].open, 4));
    CHECK(dbl_eq(candles[2].high, 4.5));
    CHECK(dbl_eq(candles[2].low, 3.9));
    CHECK(dbl_eq(candles[2].close, 4.2));
    CHECK(dbl_eq(candles[2].volume, 0)); // not final yet - seeded at zero
}

static void test_append_below_capacity_no_eviction(void)
{
    market_data_kline_t candles[8];
    candles[0] = make_candle(0, 1, 1, 1, 1, 1, 1);
    candles[1] = make_candle(INTERVAL_MS, 2, 2, 2, 2, 2, 2);
    uint16_t count = 2;

    market_data_kline_update_t u = make_update(2 * INTERVAL_MS, 3, 3, 3, 3, 1, 1, true);
    bool appended = app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &u);

    CHECK(appended == true);
    CHECK(count == 3);
    CHECK(dbl_eq(candles[0].open, 1)); // untouched
    CHECK(dbl_eq(candles[1].open, 2)); // untouched
    CHECK(candles[2].open_time_ms == 2 * INTERVAL_MS);
    CHECK(dbl_eq(candles[2].volume, 1)); // is_final - seeded from update
}

static void test_empty_series_is_noop(void)
{
    market_data_kline_t candles[8];
    uint16_t count = 0;
    market_data_kline_update_t u = make_update(0, 1, 1, 1, 1, 1, 1, true);
    bool appended = app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &u);
    CHECK(appended == false);
    CHECK(count == 0);
}

static void test_stale_bucket_is_noop(void)
{
    market_data_kline_t candles[8];
    candles[0] = make_candle(5 * INTERVAL_MS, 1, 1, 1, 1, 1, 1);
    uint16_t count = 1;

    market_data_kline_update_t u = make_update(2 * INTERVAL_MS, 9, 9, 9, 9, 9, 9, true);
    bool appended = app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &u);

    CHECK(appended == false);
    CHECK(count == 1);
    CHECK(dbl_eq(candles[0].close, 1)); // untouched
}

static void test_bucket_floor_arithmetic(void)
{
    market_data_kline_t candles[8];
    candles[0] = make_candle(0, 1, 1, 1, 1, 1, 1);
    uint16_t count = 1;

    // Not aligned to the 300000ms boundary - should floor into bucket 0,
    // i.e. merge into the existing candle rather than appending.
    market_data_kline_update_t u = make_update(123456, 2, 2, 2, 2, 1, 1, false);
    bool appended = app_state_kline_merge_apply(candles, &count, 8, INTERVAL_MS, &u);
    CHECK(appended == false);
    CHECK(count == 1);
    CHECK(dbl_eq(candles[0].close, 2));
}

static void test_append_evict_final_seeds_only_from_update(void)
{
    market_data_kline_t candles[2];
    candles[0] = make_candle(0, 1, 1, 1, 1, 500, 50); // stale values that must not leak forward
    candles[1] = make_candle(INTERVAL_MS, 2, 2, 2, 2, 300, 30);
    uint16_t count = 2;

    market_data_kline_update_t u = make_update(2 * INTERVAL_MS, 3, 3.1, 2.9, 3.05, 42, 7, true);
    bool appended = app_state_kline_merge_apply(candles, &count, 2, INTERVAL_MS, &u);

    CHECK(appended == true);
    CHECK(count == 2);
    CHECK(dbl_eq(candles[1].volume, 42)); // only from update, not leaked from evicted row 0 (500) or row 1 (300)
    CHECK(candles[1].number_of_trades == 7);
}

int main(void)
{
    test_merge_non_final_widens_high_low_and_updates_close();
    test_merge_final_accumulates_volume_and_trades_exactly_once();
    test_repeated_non_final_updates_freeze_volume_until_final();
    test_append_evicts_oldest_at_capacity();
    test_append_below_capacity_no_eviction();
    test_empty_series_is_noop();
    test_stale_bucket_is_noop();
    test_bucket_floor_arithmetic();
    test_append_evict_final_seeds_only_from_update();
    return test_summary("app_state_kline_merge");
}
