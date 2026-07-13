#pragma once

// Pure C, host-compilable: incremental (streaming) grammar for one Binance
// combined-stream WebSocket message:
//   {"stream":"btcusdt@kline_1s","data":{"e":"kline","E":...,"s":"BTCUSDT",
//    "k":{"t":...,"T":...,"s":"BTCUSDT","i":"1s","o":"...","h":"...",
//    "l":"...","c":"...","v":"...","n":...,"x":false,...}}}
//
// Like market_data_symbol_parser (not market_data_klines_parser): keys can
// arrive in any order and unrecognized keys/values (root's "stream", k's
// "f"/"L"/"q"/"V"/"Q"/"B", any future Binance addition) are structurally
// skipped rather than rejected.

#include "market_data_client.h" // market_data_err_t
#include "market_data_json_scanner.h"
#include "market_data_kline_update.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        MARKET_DATA_WS_PARSE_UPDATE = 0, // out_update fully populated - a "kline" event was parsed
        MARKET_DATA_WS_PARSE_IGNORED, // valid JSON, but not a "kline" event (e.g. no "data" key, or data.e != "kline")
        MARKET_DATA_WS_PARSE_ERROR,   // malformed/truncated JSON, or a "kline" event missing a required field
    } market_data_ws_parse_result_t;

    typedef enum
    {
        MARKET_DATA_WSP_EXPECT_ROOT_OBJ_START = 0,
        MARKET_DATA_WSP_EXPECT_ROOT_KEY_OR_END,
        MARKET_DATA_WSP_EXPECT_ROOT_COLON,
        MARKET_DATA_WSP_EXPECT_ROOT_VALUE,
        MARKET_DATA_WSP_EXPECT_ROOT_COMMA_OR_END,
        MARKET_DATA_WSP_SKIP_ROOT_VALUE,

        MARKET_DATA_WSP_EXPECT_DATA_KEY_OR_END,
        MARKET_DATA_WSP_EXPECT_DATA_COLON,
        MARKET_DATA_WSP_EXPECT_DATA_VALUE,
        MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END,
        MARKET_DATA_WSP_SKIP_DATA_VALUE,

        MARKET_DATA_WSP_EXPECT_K_KEY_OR_END,
        MARKET_DATA_WSP_EXPECT_K_COLON,
        MARKET_DATA_WSP_EXPECT_K_VALUE,
        MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END,
        MARKET_DATA_WSP_SKIP_K_VALUE,

        MARKET_DATA_WSP_DONE,
        MARKET_DATA_WSP_ERROR,
    } market_data_ws_stream_parser_state_t;

    typedef enum
    {
        MARKET_DATA_WSP_KEY_OTHER = 0,
        MARKET_DATA_WSP_KEY_DATA,       // root: "data"
        MARKET_DATA_WSP_KEY_EVENT_TYPE, // data: "e"
        MARKET_DATA_WSP_KEY_SYMBOL,     // data: "s"
        MARKET_DATA_WSP_KEY_K,          // data: "k"
        MARKET_DATA_WSP_KEY_OPEN_TIME,  // k: "t"
        MARKET_DATA_WSP_KEY_OPEN,       // k: "o"
        MARKET_DATA_WSP_KEY_HIGH,       // k: "h"
        MARKET_DATA_WSP_KEY_LOW,        // k: "l"
        MARKET_DATA_WSP_KEY_CLOSE,      // k: "c"
        MARKET_DATA_WSP_KEY_VOLUME,     // k: "v"
        MARKET_DATA_WSP_KEY_NUM_TRADES, // k: "n"
        MARKET_DATA_WSP_KEY_IS_FINAL,   // k: "x"
    } market_data_ws_stream_parser_key_t;

    typedef struct
    {
        market_data_json_scanner_t scanner;
        market_data_ws_stream_parser_state_t state;
        market_data_ws_stream_parser_key_t pending_key;
        int skip_depth;

        bool saw_e_kline;                                            // data.e == "kline" was seen
        bool saw_symbol;                                             // data.s was seen
        bool saw_k;                                                  // data.k's object was seen and fully validated
        bool saw_t, saw_o, saw_h, saw_l, saw_c, saw_v, saw_n, saw_x; // required k fields

        market_data_kline_update_t *out_update;
    } market_data_ws_stream_parser_t;

    void market_data_ws_stream_parser_init(market_data_ws_stream_parser_t *p, market_data_kline_update_t *out_update);

    // Same feed contract as the REST parsers: consumes what it can, returns
    // MARKET_DATA_ERR_PARSE only on a structural error (state -> ERROR, parser
    // must not be reused afterward).
    market_data_err_t market_data_ws_stream_parser_feed(market_data_ws_stream_parser_t *p, const char *buf, size_t len);

    // Call once input is exhausted. Only meaningful if feed() never returned an
    // error.
    market_data_ws_parse_result_t market_data_ws_stream_parser_finish(const market_data_ws_stream_parser_t *p);

#ifdef __cplusplus
}
#endif
