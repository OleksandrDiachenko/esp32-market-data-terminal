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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
    uint8_t invalid_symbol_count;   // consecutive MARKET_DATA_ERR_SYMBOL_NOT_FOUND count, reset to 0 on
                                     // success or a region change - see app_state_retry_invalid_symbol_is_recoverable
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
// real data on app_state_sync_task's next sweep (it re-reads
// app_state_symbol_count() every cycle, so this is not gated on a reboot).
// app_state_ws_task also reacts live to the watchlist-event queue below to
// subscribe/unsubscribe the WebSocket stream without a reboot - see
// docs/decisions/0007-watchlist-management.md.

// Appends ticker as a new watchlist slot (allocating its PSRAM klines
// buffer). Fails with ESP_ERR_NO_MEM if the watchlist is already at
// APP_STATE_MAX_SYMBOLS - callers should disable their own "add" entry
// point at the limit rather than rely on this as the only guard.
esp_err_t app_state_add_symbol(const char *ticker);

// Removes watchlist index (0..app_state_symbol_count()-1), freeing its
// PSRAM klines buffer and shifting later entries down - the table has no
// gaps.
esp_err_t app_state_remove_symbol(uint8_t index);

// Moves the symbol at from_index to to_index (both
// 0..app_state_symbol_count()-1), shifting the slots between them by one -
// a pure permutation, unlike add/remove it never changes
// app_state_symbol_count() nor allocates/frees a klines buffer. A no-op
// (returns ESP_OK) if from_index == to_index.
//
// Deliberately does NOT push a watchlist event: WS subscriptions are keyed
// by symbol name string, not index (see app_state_ws_task.c's
// find_index_by_symbol(), which linear-scans the live table by name on
// every tick), so reordering is fully transparent to the WS/REST layers -
// see docs/decisions/0007-watchlist-management.md and
// 0008-watchlist-live-resubscribe.md. As with add/remove, the caller still
// owns persisting the change via settings_store_save_symbols().
esp_err_t app_state_move_symbol(uint8_t from_index, uint8_t to_index);

// --- watchlist-change notifications (sole consumer: app_state_ws_task) ---

typedef enum
{
    APP_STATE_WATCHLIST_SYMBOL_ADDED,
    APP_STATE_WATCHLIST_SYMBOL_REMOVED,
    // No symbol payload - api_region_settings_t changed (Settings > Time >
    // Region, auto or manual), so the WS connection's host needs a
    // reconnect. See docs/decisions/0009-regional-server-auto-selection.md.
    APP_STATE_REGION_CHANGED,
} app_state_watchlist_event_kind_t;

typedef struct
{
    app_state_watchlist_event_kind_t kind;
    char symbol[SETTINGS_SYMBOL_MAX_LEN + 1]; // unused for APP_STATE_REGION_CHANGED
} app_state_watchlist_event_t;

// Depth sized for a person tapping add/remove far faster than any consumer
// could actually be starved - generous headroom, not a tight fit (mirrors
// MARKET_DATA_WS_UPDATE_QUEUE_LEN's own reasoning).
#define APP_STATE_WATCHLIST_EVENT_QUEUE_LEN 8

// Pushed by app_state_add_symbol()/app_state_remove_symbol() after the
// mutation is committed. Point-to-point, not broadcast - see
// market_data_ws_client_get_update_queue()'s doc comment for why only one
// task may drain this.
QueueHandle_t app_state_get_watchlist_event_queue(void);

// Pushes an APP_STATE_REGION_CHANGED event onto the same queue. Called by
// display_ui.c after api_region_settings_t changes (auto-derived from a
// new tz_label, or a manual Settings > Time > Region choice) so
// app_state_ws_task can reconnect against the new region's host - see
// docs/decisions/0009-regional-server-auto-selection.md.
void app_state_notify_region_changed(void);

// --- writer API (app_state_sync_task only) ---

// Records a successful fetch: replaces index's klines, sets state SYNCED,
// resets retry_attempt, stamps last_sync_time_ms.
esp_err_t app_state_record_success(uint8_t index, const market_data_kline_t *klines, uint16_t count,
                                    int64_t now_ms);

// Records a failed fetch. recoverable selects DEGRADED (retry_attempt++)
// vs ERROR (retry_attempt reset to 0, since retrying an unrecoverable error
// unchanged is pointless). When err is MARKET_DATA_ERR_SYMBOL_NOT_FOUND,
// invalid_symbol_count is incremented regardless of recoverable - the caller
// (app_state_sync_task, via app_state_retry_invalid_symbol_is_recoverable)
// decides when that count crosses APP_STATE_MAX_INVALID_SYMBOL_ATTEMPTS and
// passes recoverable=false to make the ERROR transition final for the
// session.
esp_err_t app_state_record_error(uint8_t index, market_data_err_t err, bool recoverable);

// Resets every watchlist symbol to a clean pre-sync state: state -> INIT,
// last_error -> MARKET_DATA_OK, retry_attempt/invalid_symbol_count -> 0,
// kline_count -> 0 (the klines buffer itself is left allocated and
// untouched - kline_count 0 is enough to make the UI show "Loading..."
// again). Klines buffers are not freed/reallocated. Called by display_ui.c
// when a Region or Time Zone change actually changes the resolved server
// (region_new != region_old), since the old host's history and any
// session-long "unsupported on this server" verdicts no longer apply to the
// new host - see docs/decisions/0009-regional-server-auto-selection.md.
esp_err_t app_state_reset_symbols_for_region_change(void);

// --- OTA update-available flag (written by app_state_ota_task) ---

#define APP_STATE_OTA_VERSION_MAX_LEN 32

typedef struct
{
    bool update_available;
    char latest_version[APP_STATE_OTA_VERSION_MAX_LEN]; // valid when update_available
    int64_t last_check_ms; // esp_timer_get_time()/1000 when this result was recorded; 0 = never checked
} app_state_ota_info_t;

// Thread-safe copy of the last OTA check's result. All-zero
// (update_available=false, empty latest_version, last_check_ms=0) until the
// first check completes - not itself an error.
esp_err_t app_state_get_ota_info(app_state_ota_info_t *out_info);

// Writer API - called by app_state_ota_task's periodic background check and
// by the Settings > Updates screen's manual "Check for updates" action.
// Stamps last_check_ms internally (esp_timer_get_time()/1000).
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
