#include "test_common.h"
#include "market_data_ws_control.h"

static void test_subscribe_message_lowercased(void)
{
    char out[128];
    market_data_err_t err =
        market_data_ws_build_control_message("SUBSCRIBE", "BTCUSDT", "kline_1s", 1, out, sizeof(out));
    CHECK(err == MARKET_DATA_OK);
    CHECK_STREQ(out, "{\"method\":\"SUBSCRIBE\",\"params\":[\"btcusdt@kline_1s\"],\"id\":1}");
}

static void test_unsubscribe_message(void)
{
    char out[128];
    market_data_err_t err =
        market_data_ws_build_control_message("UNSUBSCRIBE", "ETHUSDT", "kline_1s", 42, out, sizeof(out));
    CHECK(err == MARKET_DATA_OK);
    CHECK_STREQ(out, "{\"method\":\"UNSUBSCRIBE\",\"params\":[\"ethusdt@kline_1s\"],\"id\":42}");
}

static void test_rejects_undersized_buffer(void)
{
    char out[8];
    CHECK(market_data_ws_build_control_message("SUBSCRIBE", "BTCUSDT", "kline_1s", 1, out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_rejects_non_alnum_symbol(void)
{
    char out[128];
    CHECK(market_data_ws_build_control_message("SUBSCRIBE", "BTC/USDT", "kline_1s", 1, out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_rejects_empty_symbol(void)
{
    char out[128];
    CHECK(market_data_ws_build_control_message("SUBSCRIBE", "", "kline_1s", 1, out, sizeof(out)) ==
          MARKET_DATA_ERR_INVALID_ARG);
}

static void test_rejects_oversized_symbol(void)
{
    char out[128];
    CHECK(market_data_ws_build_control_message("SUBSCRIBE", "TOOLONGTOFITTHISSYMBOLFIELD", "kline_1s", 1, out,
                                               sizeof(out)) == MARKET_DATA_ERR_INVALID_ARG);
}

int main(void)
{
    test_subscribe_message_lowercased();
    test_unsubscribe_message();
    test_rejects_undersized_buffer();
    test_rejects_non_alnum_symbol();
    test_rejects_empty_symbol();
    test_rejects_oversized_symbol();
    return test_summary("market_data_ws_control");
}
