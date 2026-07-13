#pragma once

// Pure C, host-compilable: incremental (streaming) grammar for Binance's
// GET /api/v3/exchangeInfo?symbol=<SYM> response. Unlike the klines grammar,
// this walks a real JSON object with unpredictable field order and order-
// unbounded nested values (filters[], rateLimits[], exchangeFilters[]) that
// must be structurally skipped without being decoded. Only two fields are
// ever captured: symbols[0].status and symbols[0].permissionSets. Parsing
// stops (state -> DONE) as soon as symbols[0]'s object closes - nothing
// after it in the response is inspected.

#include "market_data_client.h"
#include "market_data_json_scanner.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        MARKET_DATA_SYP_EXPECT_ROOT_OBJ_START = 0,
        MARKET_DATA_SYP_EXPECT_ROOT_KEY_OR_END,
        MARKET_DATA_SYP_EXPECT_ROOT_COLON,
        MARKET_DATA_SYP_EXPECT_ROOT_VALUE,
        MARKET_DATA_SYP_EXPECT_ROOT_COMMA_OR_END,
        MARKET_DATA_SYP_SKIP_ROOT_VALUE,

        MARKET_DATA_SYP_EXPECT_SYMBOL0_START_OR_ARR_END,
        MARKET_DATA_SYP_EXPECT_SYMBOL_KEY_OR_END,
        MARKET_DATA_SYP_EXPECT_SYMBOL_COLON,
        MARKET_DATA_SYP_EXPECT_SYMBOL_VALUE,
        MARKET_DATA_SYP_EXPECT_SYMBOL_COMMA_OR_END,
        MARKET_DATA_SYP_SKIP_SYMBOL_VALUE,

        MARKET_DATA_SYP_EXPECT_PERMSET_START_OR_ARR_END,
        MARKET_DATA_SYP_EXPECT_PERM_STRING_OR_ARR_END,
        MARKET_DATA_SYP_EXPECT_PERM_COMMA_OR_ARR_END,
        MARKET_DATA_SYP_EXPECT_PERMSETS_COMMA_OR_ARR_END,

        MARKET_DATA_SYP_DONE,
        MARKET_DATA_SYP_ERROR,
    } market_data_symbol_parser_state_t;

    typedef enum
    {
        MARKET_DATA_SYP_KEY_OTHER = 0,
        MARKET_DATA_SYP_KEY_SYMBOLS,
        MARKET_DATA_SYP_KEY_STATUS,
        MARKET_DATA_SYP_KEY_PERMISSION_SETS,
    } market_data_symbol_parser_key_t;

    typedef struct
    {
        market_data_json_scanner_t scanner;
        market_data_symbol_parser_state_t state;
        market_data_symbol_parser_key_t pending_key; // set when a key string was just read
        int skip_depth;                              // bracket/brace nesting while skipping a value
        bool saw_status;
        market_data_symbol_status_t *out_status;
    } market_data_symbol_parser_t;

    void market_data_symbol_parser_init(market_data_symbol_parser_t *p, market_data_symbol_status_t *out_status);

    // Same feed/finish contract as market_data_klines_parser: feed() consumes
    // what it can and returns MARKET_DATA_OK unless a structural error is
    // detected; finish() reports whether the document ended in a valid state
    // (symbols[0]'s object closed with a "status" field seen).
    market_data_err_t market_data_symbol_parser_feed(market_data_symbol_parser_t *p, const char *buf, size_t len);
    market_data_err_t market_data_symbol_parser_finish(const market_data_symbol_parser_t *p);

#ifdef __cplusplus
}
#endif
