#pragma once

// Pure C, host-compilable: incremental (streaming) grammar for Binance's
// GET /api/v3/ticker/24hr?symbol=<SYM> response. Unlike the exchangeInfo
// grammar, this is a single flat object with unpredictable field order and no
// nested arrays to walk - every field is either one of the four decimal
// strings this parser captures, or a scalar (string/number) that gets
// structurally skipped. Parsing stops (state -> DONE) once the root object
// closes with all four target fields seen.

#include "market_data_client.h"
#include "market_data_json_scanner.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        MARKET_DATA_TIP_EXPECT_OBJ_START = 0,
        MARKET_DATA_TIP_EXPECT_KEY_OR_END,
        MARKET_DATA_TIP_EXPECT_COLON,
        MARKET_DATA_TIP_EXPECT_VALUE,
        MARKET_DATA_TIP_EXPECT_COMMA_OR_END,
        MARKET_DATA_TIP_SKIP_VALUE,

        MARKET_DATA_TIP_DONE,
        MARKET_DATA_TIP_ERROR,
    } market_data_ticker_parser_state_t;

    typedef enum
    {
        MARKET_DATA_TIP_KEY_OTHER = 0,
        MARKET_DATA_TIP_KEY_LAST_PRICE,
        MARKET_DATA_TIP_KEY_PRICE_CHANGE_PERCENT,
        MARKET_DATA_TIP_KEY_HIGH_PRICE,
        MARKET_DATA_TIP_KEY_LOW_PRICE,
    } market_data_ticker_parser_key_t;

    typedef struct
    {
        market_data_json_scanner_t scanner;
        market_data_ticker_parser_state_t state;
        market_data_ticker_parser_key_t pending_key;
        int skip_depth;
        bool saw_last_price;
        bool saw_price_change_percent;
        bool saw_high_price;
        bool saw_low_price;
        market_data_ticker_24hr_t *out_ticker;
    } market_data_ticker_parser_t;

    void market_data_ticker_parser_init(market_data_ticker_parser_t *p, market_data_ticker_24hr_t *out_ticker);

    // Same feed/finish contract as market_data_symbol_parser: feed() consumes
    // what it can and returns MARKET_DATA_OK unless a structural error is
    // detected; finish() reports whether the document ended in a valid state
    // (root object closed with all four target fields seen).
    market_data_err_t market_data_ticker_parser_feed(market_data_ticker_parser_t *p, const char *buf, size_t len);
    market_data_err_t market_data_ticker_parser_finish(const market_data_ticker_parser_t *p);

#ifdef __cplusplus
}
#endif
