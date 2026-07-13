#include "test_common.h"
#include "market_data_ws_stream_parser.h"

static const char *KLINE_EVENT_NOT_FINAL =
    "{\"stream\":\"btcusdt@kline_1s\",\"data\":{\"e\":\"kline\",\"E\":1672515782136,\"s\":\"BTCUSDT\","
    "\"k\":{\"t\":1672515780000,\"T\":1672515780999,\"s\":\"BTCUSDT\",\"i\":\"1s\",\"f\":100,\"L\":200,"
    "\"o\":\"0.0010\",\"c\":\"0.0020\",\"h\":\"0.0025\",\"l\":\"0.0015\",\"v\":\"1000\",\"n\":100,"
    "\"x\":false,\"q\":\"1.0000\",\"V\":\"500\",\"Q\":\"0.500\",\"B\":\"123456\"}}}";

static const char *KLINE_EVENT_FINAL =
    "{\"stream\":\"btcusdt@kline_1s\",\"data\":{\"e\":\"kline\",\"E\":1672515782136,\"s\":\"BTCUSDT\","
    "\"k\":{\"t\":1672515780000,\"T\":1672515780999,\"s\":\"BTCUSDT\",\"i\":\"1s\","
    "\"o\":\"0.0010\",\"c\":\"0.0020\",\"h\":\"0.0025\",\"l\":\"0.0015\",\"v\":\"1000\",\"n\":100,"
    "\"x\":true}}}";

static bool dbl_eq(double a, double b)
{
    double diff = a - b;
    if (diff < 0)
    {
        diff = -diff;
    }
    return diff < 1e-9;
}

static market_data_ws_parse_result_t parse_one_chunk(const char *json, market_data_kline_update_t *out)
{
    market_data_ws_stream_parser_t p;
    market_data_ws_stream_parser_init(&p, out);
    if (market_data_ws_stream_parser_feed(&p, json, strlen(json)) != MARKET_DATA_OK)
    {
        return MARKET_DATA_WS_PARSE_ERROR;
    }
    return market_data_ws_stream_parser_finish(&p);
}

static market_data_ws_parse_result_t parse_byte_by_byte(const char *json, market_data_kline_update_t *out)
{
    market_data_ws_stream_parser_t p;
    market_data_ws_stream_parser_init(&p, out);
    size_t len = strlen(json);
    for (size_t i = 0; i < len; i++)
    {
        if (market_data_ws_stream_parser_feed(&p, &json[i], 1) != MARKET_DATA_OK)
        {
            return MARKET_DATA_WS_PARSE_ERROR;
        }
    }
    return market_data_ws_stream_parser_finish(&p);
}

// Deterministic pseudo-random chunk splitter (no external RNG dependency) -
// mirrors test_market_data_klines_parser.c's chunk-boundary equivalence check.
static market_data_ws_parse_result_t parse_random_chunks(const char *json, market_data_kline_update_t *out,
                                                         unsigned seed)
{
    market_data_ws_stream_parser_t p;
    market_data_ws_stream_parser_init(&p, out);
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
        if (market_data_ws_stream_parser_feed(&p, &json[pos], chunk) != MARKET_DATA_OK)
        {
            return MARKET_DATA_WS_PARSE_ERROR;
        }
        pos += chunk;
    }
    return market_data_ws_stream_parser_finish(&p);
}

static void test_valid_kline_event(void)
{
    market_data_kline_update_t u;
    CHECK(parse_one_chunk(KLINE_EVENT_NOT_FINAL, &u) == MARKET_DATA_WS_PARSE_UPDATE);
    CHECK_STREQ(u.symbol, "BTCUSDT");
    CHECK(u.open_time_ms == 1672515780000LL);
    CHECK(dbl_eq(u.open, 0.0010));
    CHECK(dbl_eq(u.high, 0.0025));
    CHECK(dbl_eq(u.low, 0.0015));
    CHECK(dbl_eq(u.close, 0.0020));
    CHECK(dbl_eq(u.volume, 1000));
    CHECK(u.number_of_trades == 100);
    CHECK(u.is_final == false);
}

static void test_is_final_true(void)
{
    market_data_kline_update_t u;
    CHECK(parse_one_chunk(KLINE_EVENT_FINAL, &u) == MARKET_DATA_WS_PARSE_UPDATE);
    CHECK(u.is_final == true);
}

static void test_chunking_produces_identical_results(void)
{
    market_data_kline_update_t a, b, c, d;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    memset(&d, 0, sizeof(d));

    CHECK(parse_one_chunk(KLINE_EVENT_NOT_FINAL, &a) == MARKET_DATA_WS_PARSE_UPDATE);
    CHECK(parse_byte_by_byte(KLINE_EVENT_NOT_FINAL, &b) == MARKET_DATA_WS_PARSE_UPDATE);
    CHECK(parse_random_chunks(KLINE_EVENT_NOT_FINAL, &c, 1) == MARKET_DATA_WS_PARSE_UPDATE);
    CHECK(parse_random_chunks(KLINE_EVENT_NOT_FINAL, &d, 42) == MARKET_DATA_WS_PARSE_UPDATE);

    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(memcmp(&a, &c, sizeof(a)) == 0);
    CHECK(memcmp(&a, &d, sizeof(a)) == 0);
}

static void test_malformed_json_rejected(void)
{
    market_data_kline_update_t u;
    CHECK(parse_one_chunk("not json", &u) == MARKET_DATA_WS_PARSE_ERROR);
}

static void test_truncated_json_rejected(void)
{
    market_data_kline_update_t u;
    CHECK(parse_one_chunk("{\"stream\":\"btcusdt@kline_1s\",\"data\":{\"e\":\"kline\"", &u) ==
          MARKET_DATA_WS_PARSE_ERROR);
}

static void test_non_kline_event_ignored(void)
{
    const char *json = "{\"stream\":\"btcusdt@trade\",\"data\":{\"e\":\"trade\",\"E\":123,\"s\":\"BTCUSDT\"}}";
    market_data_kline_update_t u;
    CHECK(parse_one_chunk(json, &u) == MARKET_DATA_WS_PARSE_IGNORED);
}

static void test_missing_data_key_ignored(void)
{
    const char *json = "{\"stream\":\"btcusdt@kline_1s\"}";
    market_data_kline_update_t u;
    CHECK(parse_one_chunk(json, &u) == MARKET_DATA_WS_PARSE_IGNORED);
}

static void test_unknown_key_inside_k_skipped(void)
{
    const char *json =
        "{\"stream\":\"btcusdt@kline_1s\",\"data\":{\"e\":\"kline\",\"s\":\"BTCUSDT\","
        "\"k\":{\"t\":1,\"o\":\"1\",\"h\":\"2\",\"l\":\"0.5\",\"c\":\"1.5\",\"v\":\"10\",\"n\":1,\"x\":true,"
        "\"futureField\":{\"nested\":[1,2,3]}}}}";
    market_data_kline_update_t u;
    CHECK(parse_one_chunk(json, &u) == MARKET_DATA_WS_PARSE_UPDATE);
    CHECK_STREQ(u.symbol, "BTCUSDT");
}

static void test_kline_missing_required_field_rejected(void)
{
    // "n" (number_of_trades) is missing.
    const char *json = "{\"stream\":\"btcusdt@kline_1s\",\"data\":{\"e\":\"kline\",\"s\":\"BTCUSDT\","
                       "\"k\":{\"t\":1,\"o\":\"1\",\"h\":\"2\",\"l\":\"0.5\",\"c\":\"1.5\",\"v\":\"10\",\"x\":true}}}";
    market_data_kline_update_t u;
    CHECK(parse_one_chunk(json, &u) == MARKET_DATA_WS_PARSE_ERROR);
}

static void test_type_mismatch_rejected(void)
{
    // "o" (open) as a JSON number instead of a decimal string.
    const char *json =
        "{\"stream\":\"btcusdt@kline_1s\",\"data\":{\"e\":\"kline\",\"s\":\"BTCUSDT\","
        "\"k\":{\"t\":1,\"o\":1,\"h\":\"2\",\"l\":\"0.5\",\"c\":\"1.5\",\"v\":\"10\",\"n\":1,\"x\":true}}}";
    market_data_kline_update_t u;
    CHECK(parse_one_chunk(json, &u) == MARKET_DATA_WS_PARSE_ERROR);
}

int main(void)
{
    test_valid_kline_event();
    test_is_final_true();
    test_chunking_produces_identical_results();
    test_malformed_json_rejected();
    test_truncated_json_rejected();
    test_non_kline_event_ignored();
    test_missing_data_key_ignored();
    test_unknown_key_inside_k_skipped();
    test_kline_missing_required_field_rejected();
    test_type_mismatch_rejected();
    return test_summary("market_data_ws_stream_parser");
}
