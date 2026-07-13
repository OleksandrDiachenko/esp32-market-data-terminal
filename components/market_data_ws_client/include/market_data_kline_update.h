#pragma once

// Pure C, host-compilable: one decoded Binance `{symbol}@kline_1s` stream
// event. Kept in its own header (not market_data_ws_client.h, which is
// impure - it exposes a QueueHandle_t) so app_state's pure aggregator
// (app_state_kline_merge.h) can depend on the struct without pulling in
// FreeRTOS/ESP-IDF.

#include <stdbool.h>
#include <stdint.h>

#include "settings_codec.h" // SETTINGS_SYMBOL_MAX_LEN

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        char symbol[SETTINGS_SYMBOL_MAX_LEN + 1]; // data.s, e.g. "BTCUSDT"
        int64_t open_time_ms;                     // k.t - start of this 1s kline
        double open;                              // k.o
        double high;                              // k.h
        double low;                               // k.l
        double close;                             // k.c
        double volume;                            // k.v (base asset volume)
        uint32_t number_of_trades;                // k.n
        bool is_final;                            // k.x - true once this 1s kline has closed
    } market_data_kline_update_t;

#ifdef __cplusplus
}
#endif
