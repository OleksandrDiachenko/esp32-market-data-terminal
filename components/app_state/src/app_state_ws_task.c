#include "app_state_ws_task.h"

#include <string.h>

#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "market_data_ws_client.h"
#include "wifi_manager.h"

#define WS_TASK_STACK_SIZE 4096
#define WS_TASK_PRIORITY 4
#define WS_KLINE_INTERVAL_MS (5 * 60 * 1000) // matches APP_STATE_KLINE_CAPACITY / MARKET_DATA_KLINE_INTERVAL_5M
#define WIFI_POLL_INTERVAL_MS 500

static const char *TAG = "app_state_ws";

static int find_index_by_symbol(uint8_t count, const char *symbol)
{
    for (uint8_t i = 0; i < count; i++)
    {
        app_state_symbol_meta_t meta;
        if (app_state_get_symbol_meta(i, &meta) == ESP_OK && strcmp(meta.symbol, symbol) == 0)
        {
            return i;
        }
    }
    return -1;
}

// Waits for Wi-Fi to actually be connected before the first WS connect
// attempt. Starting esp_websocket_client immediately at boot (in parallel
// with ESP-Hosted's own Wi-Fi STA bring-up over the shared SDIO transport)
// was found on hardware to trigger a heap corruption in the vendored
// esp_hosted SDIO driver under that concurrent load - see
// docs/validation/websocket-streaming-hardware-test.md. Once connected,
// esp_websocket_client's own reconnect handles any later drop on its own,
// so this wait only matters once, at boot.
static void wait_for_wifi_connected(void)
{
    wifi_manager_snapshot_t snap;
    while (wifi_manager_get_snapshot(&snap) != ESP_OK || snap.state != WIFI_MANAGER_STATE_CONNECTED)
    {
        vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_INTERVAL_MS));
    }
}

static void ws_task_fn(void *arg)
{
    (void)arg;
    uint8_t count = app_state_symbol_count();

    wait_for_wifi_connected();

    char symbol_storage[APP_STATE_MAX_SYMBOLS][SETTINGS_SYMBOL_MAX_LEN + 1];
    const char *symbols[APP_STATE_MAX_SYMBOLS];
    for (uint8_t i = 0; i < count; i++)
    {
        app_state_symbol_meta_t meta;
        app_state_get_symbol_meta(i, &meta);
        strncpy(symbol_storage[i], meta.symbol, sizeof(symbol_storage[i]));
        symbols[i] = symbol_storage[i];
    }

    esp_err_t err = market_data_ws_client_start(symbols, count);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "market_data_ws_client_start failed: %s; WS consumer task exiting", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    // Sole consumer of this queue by design - market_data_kline_update_t is
    // a point-to-point FreeRTOS queue, not a broadcast (same reasoning as
    // app_state_sync_task.c's use of wifi_manager_get_event_queue()).
    QueueHandle_t updates = market_data_ws_client_get_update_queue();
    if (updates == NULL)
    {
        ESP_LOGW(TAG, "No update queue available; WS consumer task exiting");
        vTaskDelete(NULL);
        return;
    }

    market_data_kline_update_t update;
    for (;;)
    {
        // Blocking (unlike app_state_sync_task's non-blocking drain): there
        // is nothing else for this task to do, and live updates are
        // latency-sensitive.
        if (xQueueReceive(updates, &update, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        int idx = find_index_by_symbol(count, update.symbol);
        if (idx >= 0)
        {
            app_state_apply_kline_update((uint8_t)idx, &update, WS_KLINE_INTERVAL_MS);
        }
    }
}

esp_err_t app_state_ws_task_start(void)
{
    if (app_state_symbol_count() == 0)
    {
        ESP_LOGW(TAG, "Watchlist is empty; not starting the WebSocket task");
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(ws_task_fn, "app_state_ws", WS_TASK_STACK_SIZE, NULL, WS_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
