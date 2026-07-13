#include "test_common.h"
#include "market_data_url.h"

static void test_exchange_info_url(void)
{
    char out[128];
    market_data_err_t err = market_data_url_build_exchange_info("https://api.binance.com", "BTCUSDT", out, sizeof(out));
    CHECK(err == MARKET_DATA_OK);
    CHECK_STREQ(out, "https://api.binance.com/api/v3/exchangeInfo?symbol=BTCUSDT");
}

static void test_exchange_info_rejects_empty_symbol(void)
{
    char out[128];
    CHECK(market_data_url_build_exchange_info("https://api.binance.com", "", out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
    CHECK(market_data_url_build_exchange_info("https://api.binance.com", NULL, out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_exchange_info_rejects_non_alnum_symbol(void)
{
    char out[128];
    CHECK(market_data_url_build_exchange_info("https://api.binance.com", "BTC/USDT", out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_ticker_24hr_url(void)
{
    char out[128];
    market_data_err_t err = market_data_url_build_ticker_24hr("https://api.binance.com", "LTCUSDT", out, sizeof(out));
    CHECK(err == MARKET_DATA_OK);
    CHECK_STREQ(out, "https://api.binance.com/api/v3/ticker/24hr?symbol=LTCUSDT");
}

static void test_ticker_24hr_rejects_non_alnum_symbol(void)
{
    char out[128];
    CHECK(market_data_url_build_ticker_24hr("https://api.binance.com", "LTC/USDT", out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_klines_url_minimal(void)
{
    market_data_klines_request_t req = {
        .symbol = "BTCUSDT",
        .interval = "5m",
        .limit = 0,
        .start_time_ms = 0,
        .end_time_ms = 0,
    };
    char out[160];
    CHECK(market_data_url_build_klines("https://api.binance.com", &req, out, sizeof(out)) == MARKET_DATA_OK);
    CHECK_STREQ(out, "https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=5m");
}

static void test_klines_url_with_limit(void)
{
    market_data_klines_request_t req = {
        .symbol = "BTCUSDT",
        .interval = MARKET_DATA_KLINE_INTERVAL_5M,
        .limit = MARKET_DATA_KLINES_V1_LIMIT,
        .start_time_ms = 0,
        .end_time_ms = 0,
    };
    char out[160];
    CHECK(market_data_url_build_klines("https://api.binance.com", &req, out, sizeof(out)) == MARKET_DATA_OK);
    CHECK_STREQ(out, "https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=5m&limit=288");
}

static void test_klines_url_with_start_and_end_time(void)
{
    market_data_klines_request_t req = {
        .symbol = "ETHUSDT",
        .interval = "1h",
        .limit = 100,
        .start_time_ms = 1000,
        .end_time_ms = 2000,
    };
    char out[160];
    CHECK(market_data_url_build_klines("https://api.binance.com", &req, out, sizeof(out)) == MARKET_DATA_OK);
    CHECK_STREQ(
        out, "https://api.binance.com/api/v3/klines?symbol=ETHUSDT&interval=1h&limit=100&startTime=1000&endTime=2000");
}

static void test_klines_url_rejects_limit_over_max(void)
{
    market_data_klines_request_t req = {
        .symbol = "BTCUSDT",
        .interval = "5m",
        .limit = MARKET_DATA_KLINES_MAX_LIMIT + 1,
        .start_time_ms = 0,
        .end_time_ms = 0,
    };
    char out[160];
    CHECK(market_data_url_build_klines("https://api.binance.com", &req, out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_klines_url_rejects_missing_symbol(void)
{
    market_data_klines_request_t req = {
        .symbol = NULL,
        .interval = "5m",
        .limit = 0,
        .start_time_ms = 0,
        .end_time_ms = 0,
    };
    char out[160];
    CHECK(market_data_url_build_klines("https://api.binance.com", &req, out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_klines_url_rejects_undersized_buffer(void)
{
    market_data_klines_request_t req = {
        .symbol = "BTCUSDT",
        .interval = "5m",
        .limit = 288,
        .start_time_ms = 0,
        .end_time_ms = 0,
    };
    char out[8];
    CHECK(market_data_url_build_klines("https://api.binance.com", &req, out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

int main(void)
{
    test_exchange_info_url();
    test_exchange_info_rejects_empty_symbol();
    test_exchange_info_rejects_non_alnum_symbol();
    test_ticker_24hr_url();
    test_ticker_24hr_rejects_non_alnum_symbol();
    test_klines_url_minimal();
    test_klines_url_with_limit();
    test_klines_url_with_start_and_end_time();
    test_klines_url_rejects_limit_over_max();
    test_klines_url_rejects_missing_symbol();
    test_klines_url_rejects_undersized_buffer();
    return test_summary("market_data_url");
}
