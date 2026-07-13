#pragma once

// Orchestrates app_state's live WebSocket updates: waits for Wi-Fi to
// actually be connected, then starts market_data_ws_client for the current
// watchlist and becomes the sole consumer task for its update queue,
// applying each `@kline_1s` update via app_state_apply_kline_update().
// Separate from app_state_sync_task - that task blocks on REST HTTP calls
// for seconds at a time, and folding this queue's low-latency consumption
// into it would stall live updates behind REST calls.
//
// The Wi-Fi-connected wait matters only once, at boot: starting
// esp_websocket_client immediately (racing ESP-Hosted's own Wi-Fi STA
// bring-up over the shared SDIO transport) was found on hardware to trigger
// a heap corruption in the vendored esp_hosted SDIO driver - see
// docs/validation/websocket-streaming-hardware-test.md. Once connected,
// esp_websocket_client's own reconnect handles any later drop on its own.
//
// Soft dependency like app_state_sync_task_start(): failure here only
// costs live updates between REST syncs, never fatal to the rest of the app.

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Call once, after app_state_init() has loaded the watchlist and
    // wifi_manager_init()/_start() have been called (the task itself waits for
    // an actual Wi-Fi connection before doing anything network-related).
    esp_err_t app_state_ws_task_start(void);

#ifdef __cplusplus
}
#endif
