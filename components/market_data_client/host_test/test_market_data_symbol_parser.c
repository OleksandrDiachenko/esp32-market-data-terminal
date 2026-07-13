#include "test_common.h"
#include "market_data_symbol_parser.h"

// Feeds the whole buffer in one call and returns the feed() result if it
// errors early, otherwise the finish() result.
static market_data_err_t parse_all(const char *json, market_data_symbol_status_t *out_status)
{
    market_data_symbol_parser_t p;
    market_data_symbol_parser_init(&p, out_status);

    market_data_err_t err = market_data_symbol_parser_feed(&p, json, strlen(json));
    if (err != MARKET_DATA_OK)
    {
        return err;
    }
    return market_data_symbol_parser_finish(&p);
}

static void test_trading_and_spot_permitted(void)
{
    const char *json =
        "{\"timezone\":\"UTC\",\"serverTime\":123,\"symbols\":[{\"symbol\":\"BTCUSDT\",\"status\":\"TRADING\","
        "\"baseAsset\":\"BTC\",\"quoteAsset\":\"USDT\",\"permissionSets\":[[\"SPOT\",\"MARGIN\"]]}]}";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_OK);
    CHECK(status.is_trading == true);
    CHECK(status.has_spot_permission == true);
    CHECK(market_data_symbol_is_usable(&status) == true);
}

static void test_non_trading_status(void)
{
    const char *json = "{\"symbols\":[{\"symbol\":\"BTCUSDT\",\"status\":\"BREAK\",\"permissionSets\":[[\"SPOT\"]]}]}";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_OK);
    CHECK(status.is_trading == false);
    CHECK(status.has_spot_permission == true);
    CHECK(market_data_symbol_is_usable(&status) == false);
}

static void test_spot_absent(void)
{
    const char *json = "{\"symbols\":[{\"symbol\":\"BTCUSDT\",\"status\":\"TRADING\",\"permissionSets\":[[\"MARGIN\"],["
                       "\"LEVERAGED\"]]}]}";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_OK);
    CHECK(status.is_trading == true);
    CHECK(status.has_spot_permission == false);
    CHECK(market_data_symbol_is_usable(&status) == false);
}

static void test_spot_in_non_first_inner_array(void)
{
    const char *json =
        "{\"symbols\":[{\"symbol\":\"BTCUSDT\",\"status\":\"TRADING\",\"permissionSets\":[[\"MARGIN\"],[\"SPOT\"]]}]}";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_OK);
    CHECK(status.has_spot_permission == true);
}

static void test_malformed_json_rejected(void)
{
    const char *json = "{\"symbols\":[{\"symbol\":\"BTC";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_ERR_PARSE);
}

static void test_empty_symbols_array_rejected(void)
{
    const char *json = "{\"symbols\":[]}";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_ERR_PARSE);
}

static void test_missing_status_field_rejected(void)
{
    const char *json = "{\"symbols\":[{\"symbol\":\"BTCUSDT\",\"permissionSets\":[[\"SPOT\"]]}]}";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_ERR_PARSE);
}

static void test_large_filters_array_skipped(void)
{
    const char *json = "{\"symbols\":[{\"symbol\":\"BTCUSDT\",\"filters\":["
                       "{\"filterType\":\"PRICE_FILTER\",\"minPrice\":\"0.01000000\",\"maxPrice\":\"1000000.00000000\","
                       "\"tickSize\":\"0.01000000\"},"
                       "{\"filterType\":\"LOT_SIZE\",\"minQty\":\"0.00001000\",\"maxQty\":\"9000.00000000\","
                       "\"stepSize\":\"0.00001000\"}"
                       "],\"status\":\"TRADING\",\"permissionSets\":[[\"SPOT\"]]}]}";
    market_data_symbol_status_t status;
    CHECK(parse_all(json, &status) == MARKET_DATA_OK);
    CHECK(status.is_trading == true);
    CHECK(status.has_spot_permission == true);
}

int main(void)
{
    test_trading_and_spot_permitted();
    test_non_trading_status();
    test_spot_absent();
    test_spot_in_non_first_inner_array();
    test_malformed_json_rejected();
    test_empty_symbols_array_rejected();
    test_missing_status_field_rejected();
    test_large_filters_array_skipped();
    return test_summary("market_data_symbol_parser");
}
