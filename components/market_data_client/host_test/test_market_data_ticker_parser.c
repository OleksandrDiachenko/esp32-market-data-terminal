#include "test_common.h"
#include "market_data_ticker_parser.h"

static market_data_err_t parse_all(const char *json, market_data_ticker_24hr_t *out_ticker)
{
    market_data_ticker_parser_t p;
    market_data_ticker_parser_init(&p, out_ticker);

    market_data_err_t err = market_data_ticker_parser_feed(&p, json, strlen(json));
    if (err != MARKET_DATA_OK)
    {
        return err;
    }
    return market_data_ticker_parser_finish(&p);
}

static void test_full_ticker(void)
{
    const char *json =
        "{\"symbol\":\"LTCUSDT\",\"priceChange\":\"1.71000000\",\"priceChangePercent\":\"1.852\","
        "\"weightedAvgPrice\":\"93.10000000\",\"prevClosePrice\":\"92.41000000\",\"lastPrice\":\"94.12000000\","
        "\"lastQty\":\"0.10000000\",\"bidPrice\":\"94.11000000\",\"bidQty\":\"1.00000000\","
        "\"askPrice\":\"94.13000000\",\"askQty\":\"1.00000000\",\"openPrice\":\"92.41000000\","
        "\"highPrice\":\"95.40000000\",\"lowPrice\":\"90.85000000\",\"volume\":\"12345.00000000\","
        "\"quoteVolume\":\"1150000.00000000\",\"openTime\":1000,\"closeTime\":2000,\"firstId\":1,\"lastId\":2,"
        "\"count\":3}";
    market_data_ticker_24hr_t ticker;
    CHECK(parse_all(json, &ticker) == MARKET_DATA_OK);
    CHECK(ticker.last_price == 94.12);
    CHECK(ticker.price_change_percent == 1.852);
    CHECK(ticker.high_price == 95.40);
    CHECK(ticker.low_price == 90.85);
}

static void test_field_order_independent(void)
{
    const char *json = "{\"lowPrice\":\"90.85000000\",\"highPrice\":\"95.40000000\",\"symbol\":\"LTCUSDT\","
                       "\"priceChangePercent\":\"-1.852\",\"lastPrice\":\"94.12000000\"}";
    market_data_ticker_24hr_t ticker;
    CHECK(parse_all(json, &ticker) == MARKET_DATA_OK);
    CHECK(ticker.last_price == 94.12);
    CHECK(ticker.price_change_percent == -1.852);
    CHECK(ticker.high_price == 95.40);
    CHECK(ticker.low_price == 90.85);
}

static void test_missing_field_rejected(void)
{
    const char *json = "{\"symbol\":\"LTCUSDT\",\"lastPrice\":\"94.12000000\",\"highPrice\":\"95.40000000\","
                       "\"lowPrice\":\"90.85000000\"}";
    market_data_ticker_24hr_t ticker;
    CHECK(parse_all(json, &ticker) == MARKET_DATA_ERR_PARSE);
}

static void test_malformed_json_rejected(void)
{
    const char *json = "{\"symbol\":\"LTC";
    market_data_ticker_24hr_t ticker;
    CHECK(parse_all(json, &ticker) == MARKET_DATA_ERR_PARSE);
}

static void test_binance_error_body_rejected(void)
{
    const char *json = "{\"code\":-1121,\"msg\":\"Invalid symbol.\"}";
    market_data_ticker_24hr_t ticker;
    CHECK(parse_all(json, &ticker) == MARKET_DATA_ERR_PARSE);
}

int main(void)
{
    test_full_ticker();
    test_field_order_independent();
    test_missing_field_rejected();
    test_malformed_json_rejected();
    test_binance_error_body_rejected();
    return test_summary("market_data_ticker_parser");
}
