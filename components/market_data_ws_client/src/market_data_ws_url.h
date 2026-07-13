#pragma once

// Pure C, host-compilable: builds the combined-stream WebSocket URL for a
// list of symbols, e.g. base "wss://stream.binance.com:9443" + symbols
// {"BTCUSDT","ETHUSDT"} + suffix "kline_1s" ->
// "wss://stream.binance.com:9443/stream?streams=btcusdt@kline_1s/ethusdt@kline_1s".
//
// Region selection (which base URL to pass in) is not pure - it reads
// settings_store - so it lives in market_data_ws_client.c, mirroring how
// market_data_client.c's select_base_url() is kept out of the pure
// market_data_url.c.

#include <stddef.h>
#include <stdint.h>

#include "market_data_client.h" // market_data_err_t

#ifdef __cplusplus
extern "C"
{
#endif

    market_data_err_t market_data_ws_url_build_combined_stream(const char *base_ws_url, const char *const *symbols,
                                                               uint8_t symbol_count, const char *stream_suffix,
                                                               char *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif
