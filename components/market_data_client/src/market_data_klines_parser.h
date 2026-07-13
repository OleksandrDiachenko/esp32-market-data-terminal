#pragma once

// Pure C, host-compilable: incremental (streaming) grammar for Binance's
// GET /api/v3/klines response - a bare JSON array of 12-element arrays.
// Rigid shape, no generic value-skip needed (every field is consumed).
// Rows are written directly into the caller-owned output array as each is
// completed; nothing is buffered beyond a handful of in-flight scalar
// fields. See market_data_json_scanner.h for why this is streaming rather
// than DOM-based.

#include "market_data_client.h"
#include "market_data_json_scanner.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        MARKET_DATA_KLP_EXPECT_OUTER_START = 0,
        MARKET_DATA_KLP_EXPECT_ROW_START_OR_OUTER_END,
        MARKET_DATA_KLP_EXPECT_FIELD_VALUE,
        MARKET_DATA_KLP_EXPECT_FIELD_COMMA_OR_ROW_END,
        MARKET_DATA_KLP_EXPECT_ROW_COMMA_OR_OUTER_END,
        MARKET_DATA_KLP_DONE,
        MARKET_DATA_KLP_ERROR,
    } market_data_klines_parser_state_t;

    typedef struct
    {
        market_data_json_scanner_t scanner;
        market_data_klines_parser_state_t state;
        int field_index; // 0..11 within the row currently being parsed
        market_data_kline_t *out_klines;
        uint16_t out_capacity;
        uint16_t out_count;
    } market_data_klines_parser_t;

    void market_data_klines_parser_init(market_data_klines_parser_t *p, market_data_kline_t *out_klines,
                                        uint16_t out_capacity);

    // Feeds a chunk of raw response bytes. Returns MARKET_DATA_OK if the chunk
    // was consumed without a structural error (parsing may still be in
    // progress - check market_data_klines_parser_finish() once the body is
    // fully read). Returns MARKET_DATA_ERR_PARSE or MARKET_DATA_ERR_INVALID_ARG
    // (row count would exceed out_capacity) as soon as either is detected; the
    // caller should stop reading and close the connection at that point.
    market_data_err_t market_data_klines_parser_feed(market_data_klines_parser_t *p, const char *buf, size_t len);

    // Call once the HTTP body has been fully read (no more bytes to feed).
    // Returns MARKET_DATA_OK with *out_count set if the document ended cleanly
    // (outer array closed); MARKET_DATA_ERR_PARSE if the stream ended mid-value
    // (a truncated response).
    market_data_err_t market_data_klines_parser_finish(const market_data_klines_parser_t *p, uint16_t *out_count);

#ifdef __cplusplus
}
#endif
