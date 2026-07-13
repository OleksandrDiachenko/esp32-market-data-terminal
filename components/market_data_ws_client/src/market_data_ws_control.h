#pragma once

// Pure C, host-compilable: builds a Binance WS control-frame JSON body sent
// over an already-open combined-stream connection to add/remove one stream
// without reconnecting, e.g. method "SUBSCRIBE" + symbol "BTCUSDT" + suffix
// "kline_1s" + id 1 -> {"method":"SUBSCRIBE","params":["btcusdt@kline_1s"],"id":1}
//
// This is deliberately separate from the *initial* connection's URL, which
// stays exactly as docs/decisions/0004-websocket-streaming.md decided
// (subscription-via-URL, no control frame at connect time) - control frames
// are only used for runtime watchlist edits after that connection already
// exists, see docs/decisions/0007-watchlist-management.md.

#include <stddef.h>
#include <stdint.h>

#include "market_data_client.h" // market_data_err_t

#ifdef __cplusplus
extern "C"
{
#endif

    market_data_err_t market_data_ws_build_control_message(const char *method, const char *symbol,
                                                           const char *stream_suffix, uint32_t id, char *out,
                                                           size_t out_capacity);

#ifdef __cplusplus
}
#endif
