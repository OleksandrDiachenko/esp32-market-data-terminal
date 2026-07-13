#pragma once

// Public API for the Binance public WebSocket kline stream client. Owns one
// combined-stream connection (`{sym0}@kline_1s/{sym1}@kline_1s/...`) for the
// whole watchlist and a FreeRTOS queue of decoded market_data_kline_update_t
// - the same owner-owns-queue, single-consumer pattern wifi_manager already
// uses for its own event queue (see wifi_manager.h's
// wifi_manager_get_event_queue()). No API keys, no trading logic.
//
// Region-aware endpoint selection reuses settings_store_load_api_region(),
// mirroring market_data_client.c's select_base_url(). Reconnect-with-backoff
// is esp_websocket_client's own built-in behavior, not hand-rolled here.

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "market_data_kline_update.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Depth sized for 8 watchlist symbols emitting ~1 update/s each, consumed
// near-instantly by app_state_ws_task - generous headroom, not a tight fit.
#define MARKET_DATA_WS_UPDATE_QUEUE_LEN 32

    // Builds the region-aware combined-stream URL for symbols[0..symbol_count)
    // and creates the client and its update queue (market_data_ws_client_get_
    // update_queue() is valid once this returns ESP_OK), but does not connect
    // yet - no data can arrive on the update queue until
    // market_data_ws_client_connect() is called. symbols are copied internally
    // (do not need to outlive this call). symbol_count must be
    // 1..SETTINGS_MAX_WATCHLIST.
    //
    // Split from the connect step so a caller that needs to join the update
    // queue to a FreeRTOS queue set can do so while it's still guaranteed empty
    // - xQueueAddToSet() fails if the queue already has data pending, which it
    // could the moment esp_websocket_client starts receiving ticks. See
    // app_state_ws_task.c's ws_task_fn() for the intended create-then-join-then-
    // connect sequencing.
    esp_err_t market_data_ws_client_create(const char *const *symbols, uint8_t symbol_count);

    // Begins connecting the client created by market_data_ws_client_create()
    // (auto-reconnect-with-backoff enabled). Soft dependency like Wi-Fi/
    // time_sync: returns ESP_OK once the connect attempt is started, even if it
    // later fails - esp_websocket_client keeps retrying on its own.
    esp_err_t market_data_ws_client_connect(void);

    // Convenience wrapper: market_data_ws_client_create() followed immediately
    // by market_data_ws_client_connect(), for callers that don't need to join
    // the update queue to a queue set before data can arrive.
    esp_err_t market_data_ws_client_start(const char *const *symbols, uint8_t symbol_count);

    // Stops and destroys the underlying client and its queue. Called by
    // components/app_state/src/app_state_ws_task.c to rebuild the connection
    // against a new region's host after an API region switch - see
    // docs/decisions/0009-regional-server-auto-selection.md.
    void market_data_ws_client_stop(void);

    // Sole consumer: components/app_state/src/app_state_ws_task.c. Returns NULL
    // if market_data_ws_client_start() was never called (or failed).
    QueueHandle_t market_data_ws_client_get_update_queue(void);

    // Adds/removes one `${symbol}@kline_1s` stream to/from the already-open
    // combined-stream connection via Binance's SUBSCRIBE/UNSUBSCRIBE control
    // frame (esp_websocket_client_send_text()), without reconnecting - see
    // docs/decisions/0007-watchlist-management.md. Returns
    // ESP_ERR_INVALID_STATE if market_data_ws_client_start() hasn't succeeded
    // yet, ESP_ERR_INVALID_ARG for a malformed symbol. Fire-and-forget like the
    // rest of this module's soft-dependency treatment of the WS link - a send
    // failure is logged, not retried here (esp_websocket_client's own
    // reconnect will re-establish the base connection if it was actually down).
    esp_err_t market_data_ws_client_subscribe(const char *symbol);
    esp_err_t market_data_ws_client_unsubscribe(const char *symbol);

#ifdef __cplusplus
}
#endif
