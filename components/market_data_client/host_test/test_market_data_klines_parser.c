#include "test_common.h"
#include "market_data_klines_parser.h"

static const char *KLINES_JSON_2ROWS =
    "[[1499040000000,\"0.01634790\",\"0.80000000\",\"0.01575800\",\"0.01577100\",\"148976.11427815\","
    "1499644799999,\"2434.19055334\",308,\"1756.87402397\",\"28.46694368\",\"0\"],"
    "[1499644800000,\"0.01577100\",\"0.02000000\",\"0.01500000\",\"0.01600000\",\"100.00000000\","
    "1500249599999,\"1.50000000\",5,\"0.50000000\",\"0.75000000\",\"0\"]]";

static bool dbl_eq(double a, double b)
{
    double diff = a - b;
    if (diff < 0)
    {
        diff = -diff;
    }
    return diff < 1e-6;
}

static market_data_err_t parse_one_chunk(const char *json, market_data_kline_t *out, uint16_t cap, uint16_t *out_count)
{
    market_data_klines_parser_t p;
    market_data_klines_parser_init(&p, out, cap);
    market_data_err_t err = market_data_klines_parser_feed(&p, json, strlen(json));
    if (err != MARKET_DATA_OK)
    {
        return err;
    }
    return market_data_klines_parser_finish(&p, out_count);
}

static market_data_err_t parse_byte_by_byte(const char *json, market_data_kline_t *out, uint16_t cap,
                                            uint16_t *out_count)
{
    market_data_klines_parser_t p;
    market_data_klines_parser_init(&p, out, cap);
    size_t len = strlen(json);
    for (size_t i = 0; i < len; i++)
    {
        market_data_err_t err = market_data_klines_parser_feed(&p, &json[i], 1);
        if (err != MARKET_DATA_OK)
        {
            return err;
        }
    }
    return market_data_klines_parser_finish(&p, out_count);
}

// Deterministic pseudo-random chunk splitter (no external RNG dependency).
static market_data_err_t parse_random_chunks(const char *json, market_data_kline_t *out, uint16_t cap,
                                             uint16_t *out_count, unsigned seed)
{
    market_data_klines_parser_t p;
    market_data_klines_parser_init(&p, out, cap);
    size_t len = strlen(json);
    size_t pos = 0;
    unsigned state = seed;
    while (pos < len)
    {
        state = state * 1103515245u + 12345u;
        size_t chunk = 1 + (state / 65536u) % 5; // 1..5 bytes
        if (chunk > len - pos)
        {
            chunk = len - pos;
        }
        market_data_err_t err = market_data_klines_parser_feed(&p, &json[pos], chunk);
        if (err != MARKET_DATA_OK)
        {
            return err;
        }
        pos += chunk;
    }
    return market_data_klines_parser_finish(&p, out_count);
}

static void test_valid_multirow(void)
{
    market_data_kline_t rows[8];
    uint16_t count = 0;
    CHECK(parse_one_chunk(KLINES_JSON_2ROWS, rows, 8, &count) == MARKET_DATA_OK);
    CHECK(count == 2);
    CHECK(rows[0].open_time_ms == 1499040000000LL);
    CHECK(dbl_eq(rows[0].open, 0.01634790));
    CHECK(dbl_eq(rows[0].high, 0.80000000));
    CHECK(dbl_eq(rows[0].low, 0.01575800));
    CHECK(dbl_eq(rows[0].close, 0.01577100));
    CHECK(dbl_eq(rows[0].volume, 148976.11427815));
    CHECK(rows[0].close_time_ms == 1499644799999LL);
    CHECK(dbl_eq(rows[0].quote_asset_volume, 2434.19055334));
    CHECK(rows[0].number_of_trades == 308);
    CHECK(dbl_eq(rows[0].taker_buy_base_volume, 1756.87402397));
    CHECK(dbl_eq(rows[0].taker_buy_quote_volume, 28.46694368));
    CHECK(rows[1].number_of_trades == 5);
}

static void test_chunking_produces_identical_results(void)
{
    market_data_kline_t a[8], b[8], c[8], d[8];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    memset(c, 0, sizeof(c));
    memset(d, 0, sizeof(d));
    uint16_t ca = 0, cb = 0, cc = 0, cd = 0;

    CHECK(parse_one_chunk(KLINES_JSON_2ROWS, a, 8, &ca) == MARKET_DATA_OK);
    CHECK(parse_byte_by_byte(KLINES_JSON_2ROWS, b, 8, &cb) == MARKET_DATA_OK);
    CHECK(parse_random_chunks(KLINES_JSON_2ROWS, c, 8, &cc, 1) == MARKET_DATA_OK);
    CHECK(parse_random_chunks(KLINES_JSON_2ROWS, d, 8, &cd, 42) == MARKET_DATA_OK);

    CHECK(ca == cb);
    CHECK(ca == cc);
    CHECK(ca == cd);
    for (uint16_t i = 0; i < ca; i++)
    {
        CHECK(memcmp(&a[i], &b[i], sizeof(market_data_kline_t)) == 0);
        CHECK(memcmp(&a[i], &c[i], sizeof(market_data_kline_t)) == 0);
        CHECK(memcmp(&a[i], &d[i], sizeof(market_data_kline_t)) == 0);
    }
}

static void test_malformed_json_rejected(void)
{
    market_data_kline_t rows[4];
    uint16_t count = 0;
    CHECK(parse_one_chunk("not json", rows, 4, &count) == MARKET_DATA_ERR_PARSE);
}

static void test_row_too_few_fields_rejected(void)
{
    const char *json = "[[1499040000000,\"0.1\",\"0.2\",\"0.1\",\"0.15\",\"100\",1499644799999,\"10\",5,\"1\","
                       "\"2\"]]"; // 11 fields, missing "ignore"
    market_data_kline_t rows[4];
    uint16_t count = 0;
    CHECK(parse_one_chunk(json, rows, 4, &count) == MARKET_DATA_ERR_PARSE);
}

static void test_row_too_many_fields_rejected(void)
{
    const char *json = "[[1499040000000,\"0.1\",\"0.2\",\"0.1\",\"0.15\",\"100\",1499644799999,\"10\",5,\"1\",\"2\","
                       "\"0\",\"extra\"]]"; // 13 fields
    market_data_kline_t rows[4];
    uint16_t count = 0;
    CHECK(parse_one_chunk(json, rows, 4, &count) == MARKET_DATA_ERR_PARSE);
}

static void test_decimal_field_as_number_rejected(void)
{
    const char *json = "[[1499040000000,0.1,\"0.2\",\"0.1\",\"0.15\",\"100\",1499644799999,\"10\",5,\"1\",\"2\","
                       "\"0\"]]"; // field 1 ("open") is a JSON number, not a string
    market_data_kline_t rows[4];
    uint16_t count = 0;
    CHECK(parse_one_chunk(json, rows, 4, &count) == MARKET_DATA_ERR_PARSE);
}

static void test_capacity_exceeded_rejected(void)
{
    market_data_kline_t rows[1];
    uint16_t count = 0;
    CHECK(parse_one_chunk(KLINES_JSON_2ROWS, rows, 1, &count) == MARKET_DATA_ERR_INVALID_ARG);
}

static void test_exactly_capacity_succeeds(void)
{
    market_data_kline_t rows[2];
    uint16_t count = 0;
    CHECK(parse_one_chunk(KLINES_JSON_2ROWS, rows, 2, &count) == MARKET_DATA_OK);
    CHECK(count == 2);
}

static void test_empty_array_is_ok(void)
{
    market_data_kline_t rows[4];
    uint16_t count = 123;
    CHECK(parse_one_chunk("[]", rows, 4, &count) == MARKET_DATA_OK);
    CHECK(count == 0);
}

int main(void)
{
    test_valid_multirow();
    test_chunking_produces_identical_results();
    test_malformed_json_rejected();
    test_row_too_few_fields_rejected();
    test_row_too_many_fields_rejected();
    test_decimal_field_as_number_rejected();
    test_capacity_exceeded_rejected();
    test_exactly_capacity_succeeds();
    test_empty_array_is_ok();
    return test_summary("market_data_klines_parser");
}
