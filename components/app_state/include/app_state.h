#pragma once

// Runtime state model for the watchlist's market data: one entry per symbol
// loaded from settings_store's symbol_settings_t, holding the last REST
// sync result and its 24h/5m klines. Written only by app_state_sync_task
// (single writer); read via the thread-safe accessors below. Klines
// buffers are PSRAM-backed (SETTINGS_MAX_WATCHLIST entries at
// MARKET_DATA_KLINES_V1_LIMIT candles each is too large for internal RAM
// alongside the display/LVGL buffers).
//
// This module owns *storage* only. Retry/backoff and recoverable-error
// classification are pure logic in app_state_retry_policy.h (host-testable,
// no ESP-IDF deps). Orchestration (Wi-Fi/time_sync events, calling
// market_data_client, applying the retry policy) lives in
// app_state_sync_task.h.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "market_data_client.h"
#include "settings_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_STATE_MAX_SYMBOLS SETTINGS_MAX_WATCHLIST
#define APP_STATE_KLINE_CAPACITY MARKET_DATA_KLINES_V1_LIMIT

typedef enum
{
    APP_STATE_SYMBOL_INIT = 0, // loaded from settings, never fetched yet
    APP_STATE_SYMBOL_SYNCED,   // last fetch attempt succeeded
    APP_STATE_SYMBOL_DEGRADED, // last attempt failed with a recoverable error; sync_task is retrying
    APP_STATE_SYMBOL_ERROR,    // last attempt failed with an unrecoverable error - not retried until the watchlist changes
} app_state_symbol_state_t;

typedef struct
{
    char symbol[SETTINGS_SYMBOL_MAX_LEN + 1];
    app_state_symbol_state_t state;
    market_data_err_t last_error;   // meaningful when state != APP_STATE_SYMBOL_SYNCED
    uint8_t retry_attempt;          // consecutive recoverable-failure count, reset to 0 on success
    int64_t last_sync_time_ms;      // esp_timer_get_time()/1000 at last successful fetch; 0 if never synced
    uint16_t kline_count;
} app_state_symbol_meta_t;

// Allocates PSRAM klines buffers and loads the watchlist from
// settings_store_load_symbols(). An empty watchlist (settings_store's
// default) is not an error - app_state_symbol_count() returns 0 and the
// sync task simply has nothing to do until symbols are configured.
esp_err_t app_state_init(void);

uint8_t app_state_symbol_count(void);

// Thread-safe metadata copy for watchlist index (0..app_state_symbol_count()-1).
esp_err_t app_state_get_symbol_meta(uint8_t index, app_state_symbol_meta_t *out_meta);

// Thread-safe copy of index's current klines into a caller-owned buffer.
esp_err_t app_state_get_symbol_klines(uint8_t index, market_data_kline_t *out_klines, uint16_t out_capacity,
                                       uint16_t *out_count);

// --- writer API (app_state_sync_task only) ---

// Records a successful fetch: replaces index's klines, sets state SYNCED,
// resets retry_attempt, stamps last_sync_time_ms.
esp_err_t app_state_record_success(uint8_t index, const market_data_kline_t *klines, uint16_t count,
                                    int64_t now_ms);

// Records a failed fetch. recoverable selects DEGRADED (retry_attempt++)
// vs ERROR (retry_attempt reset to 0, since retrying an unrecoverable error
// unchanged is pointless).
esp_err_t app_state_record_error(uint8_t index, market_data_err_t err, bool recoverable);

#ifdef __cplusplus
}
#endif
