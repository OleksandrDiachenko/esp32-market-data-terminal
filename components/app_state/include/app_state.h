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
#include "market_data_kline_update.h"
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

// --- watchlist editing (display_ui's Settings screens) ---
//
// Runtime-only: the caller is responsible for also persisting the change via
// settings_store_save_symbols() - app_state doesn't call back into
// settings_store itself. New symbols start at APP_STATE_SYMBOL_INIT and get
// real data on app_state_sync_task's next sweep; see
// docs/decisions/0007-watchlist-management.md for why the live WebSocket
// stream doesn't pick them up until the next reboot.

// Appends ticker as a new watchlist slot (allocating its PSRAM klines
// buffer). Fails with ESP_ERR_NO_MEM if the watchlist is already at
// APP_STATE_MAX_SYMBOLS - callers should disable their own "add" entry
// point at the limit rather than rely on this as the only guard.
esp_err_t app_state_add_symbol(const char *ticker);

// Removes watchlist index (0..app_state_symbol_count()-1), freeing its
// PSRAM klines buffer and shifting later entries down - the table has no
// gaps.
esp_err_t app_state_remove_symbol(uint8_t index);

// --- writer API (app_state_sync_task only) ---

// Records a successful fetch: replaces index's klines, sets state SYNCED,
// resets retry_attempt, stamps last_sync_time_ms.
esp_err_t app_state_record_success(uint8_t index, const market_data_kline_t *klines, uint16_t count,
                                    int64_t now_ms);

// Records a failed fetch. recoverable selects DEGRADED (retry_attempt++)
// vs ERROR (retry_attempt reset to 0, since retrying an unrecoverable error
// unchanged is pointless).
esp_err_t app_state_record_error(uint8_t index, market_data_err_t err, bool recoverable);

// --- OTA update-available flag (written by app_state_ota_task) ---

#define APP_STATE_OTA_VERSION_MAX_LEN 32

typedef struct
{
    bool update_available;
    char latest_version[APP_STATE_OTA_VERSION_MAX_LEN]; // valid when update_available
} app_state_ota_info_t;

// Thread-safe copy of the last background OTA check's result. All-zero
// (update_available=false, empty latest_version) until the first check
// completes - not itself an error.
esp_err_t app_state_get_ota_info(app_state_ota_info_t *out_info);

// Writer API (app_state_ota_task only).
esp_err_t app_state_set_ota_info(bool update_available, const char *latest_version);

// Applies one live `@kline_1s` update from app_state_ws_task into index's
// klines (see app_state_kline_merge.h for the merge/append/stale-ignore
// rules), under the same lock as app_state_record_success()/_error(). Does
// NOT touch state/retry_attempt/last_error - those remain owned exclusively
// by the REST sync task. A no-op if index's klines are still empty (REST
// hasn't bootstrapped this symbol yet).
esp_err_t app_state_apply_kline_update(uint8_t index, const market_data_kline_update_t *update, int64_t interval_ms);

#ifdef __cplusplus
}
#endif
