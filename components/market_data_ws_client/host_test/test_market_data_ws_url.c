#include "test_common.h"
#include "market_data_ws_url.h"

static void test_single_symbol_lowercased(void)
{
    const char *symbols[] = {"BTCUSDT"};
    char out[128];
    market_data_err_t err = market_data_ws_url_build_combined_stream("wss://stream.binance.com:9443", symbols, 1,
                                                                     "kline_1s", out, sizeof(out));
    CHECK(err == MARKET_DATA_OK);
    CHECK_STREQ(out, "wss://stream.binance.com:9443/stream?streams=btcusdt@kline_1s");
}

static void test_multiple_symbols_joined_in_order(void)
{
    const char *symbols[] = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    char out[160];
    market_data_err_t err = market_data_ws_url_build_combined_stream("wss://stream.binance.com:9443", symbols, 3,
                                                                     "kline_1s", out, sizeof(out));
    CHECK(err == MARKET_DATA_OK);
    CHECK_STREQ(out, "wss://stream.binance.com:9443/stream?streams="
                     "btcusdt@kline_1s/ethusdt@kline_1s/solusdt@kline_1s");
}

static void test_rejects_undersized_buffer(void)
{
    const char *symbols[] = {"BTCUSDT"};
    char out[8];
    CHECK(market_data_ws_url_build_combined_stream("wss://stream.binance.com:9443", symbols, 1, "kline_1s", out,
                                                   sizeof(out)) == MARKET_DATA_ERR_INVALID_ARG);
}

static void test_rejects_non_alnum_symbol(void)
{
    const char *symbols[] = {"BTC/USDT"};
    char out[128];
    CHECK(market_data_ws_url_build_combined_stream("wss://stream.binance.com:9443", symbols, 1, "kline_1s", out,
                                                   sizeof(out)) == MARKET_DATA_ERR_INVALID_ARG);
}

static void test_rejects_zero_symbol_count(void)
{
    const char *symbols[] = {"BTCUSDT"};
    char out[128];
    CHECK(market_data_ws_url_build_combined_stream("wss://stream.binance.com:9443", symbols, 0, "kline_1s", out,
                                                   sizeof(out)) == MARKET_DATA_ERR_INVALID_ARG);
}

int main(void)
{
    test_single_symbol_lowercased();
    test_multiple_symbols_joined_in_order();
    test_rejects_undersized_buffer();
    test_rejects_non_alnum_symbol();
    test_rejects_zero_symbol_count();
    return test_summary("market_data_ws_url");
}
