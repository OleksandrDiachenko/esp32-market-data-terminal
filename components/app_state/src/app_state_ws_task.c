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

// Looks up against the *current* app_state_symbol_count(), not a count
// snapshotted at task start - a symbol added at runtime is appended past
// whatever count was frozen at boot, so a stale bound here would silently
// drop its live ticks even after market_data_ws_client_subscribe() picks it
// up on the wire.
static int find_index_by_symbol(const char *symbol)
{
    uint8_t count = app_state_symbol_count();
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

// Creates the client for the current watchlist, joins its update queue to
// queue_set (if non-NULL), and connects - the same create -> join-to-set ->
// connect ordering used at task start (see ws_task_fn()'s own doc comment
// on why that order matters), reused here so a region switch can rebuild
// the connection against the new host without racing the heap-corruption
// bug. Returns the new update queue, or NULL on failure.
static QueueHandle_t start_ws_client(QueueSetHandle_t queue_set)
{
    uint8_t count = app_state_symbol_count();
    if (count == 0)
    {
        return NULL;
    }
    char symbol_storage[APP_STATE_MAX_SYMBOLS][SETTINGS_SYMBOL_MAX_LEN + 1];
    const char *symbols[APP_STATE_MAX_SYMBOLS];
    for (uint8_t i = 0; i < count; i++)
    {
        app_state_symbol_meta_t meta;
        app_state_get_symbol_meta(i, &meta);
        strncpy(symbol_storage[i], meta.symbol, sizeof(symbol_storage[i]));
        symbols[i] = symbol_storage[i];
    }

    esp_err_t err = market_data_ws_client_create(symbols, count);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "market_data_ws_client_create failed: %s", esp_err_to_name(err));
        return NULL;
    }

    QueueHandle_t updates = market_data_ws_client_get_update_queue();
    if (updates == NULL)
    {
        ESP_LOGE(TAG, "No update queue after create");
        market_data_ws_client_stop();
        return NULL;
    }
    if (queue_set != NULL && xQueueAddToSet(updates, queue_set) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to join update queue to the queue set");
        market_data_ws_client_stop();
        return NULL;
    }

    err = market_data_ws_client_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "market_data_ws_client_connect failed: %s", esp_err_to_name(err));
        if (queue_set != NULL)
        {
            xQueueRemoveFromSet(updates, queue_set);
        }
        market_data_ws_client_stop();
        return NULL;
    }
    return updates;
}

static void ws_task_fn(void *arg)
{
    (void)arg;

    wait_for_wifi_connected();

    // Also sole consumer of this one - see its own doc comment. A queue
    // set lets this task block on whichever of the two fires first instead
    // of picking one to poll and the other to block on.
    QueueHandle_t watchlist_events = app_state_get_watchlist_event_queue();
    QueueSetHandle_t queue_set = xQueueCreateSet(MARKET_DATA_WS_UPDATE_QUEUE_LEN + APP_STATE_WATCHLIST_EVENT_QUEUE_LEN);
    if (queue_set == NULL || watchlist_events == NULL || xQueueAddToSet(watchlist_events, queue_set) != pdPASS)
    {
        ESP_LOGE(TAG, "Queue set setup failed; watchlist edits won't live-resubscribe until reboot");
        queue_set = NULL;
    }

    QueueHandle_t updates = NULL;
    if (app_state_symbol_count() > 0)
    {
        updates = start_ws_client(queue_set);
        if (updates == NULL && queue_set == NULL)
        {
            ESP_LOGW(TAG, "WS client start failed; WS consumer task exiting");
            vTaskDelete(NULL);
            return;
        }
    }
    else
    {
        ESP_LOGI(TAG, "Watchlist is empty; waiting for the first symbol before starting WebSocket");
    }
    if (queue_set == NULL && updates == NULL)
    {
        ESP_LOGE(TAG, "Cannot observe watchlist changes without a queue set; WS consumer task exiting");
        vTaskDelete(NULL);
        return;
    }

    market_data_kline_update_t update;
    app_state_watchlist_event_t watchlist_event;
    for (;;)
    {
        if (queue_set == NULL)
        {
            // Fallback: behave exactly as before this feature existed.
            if (xQueueReceive(updates, &update, portMAX_DELAY) == pdTRUE)
            {
                int idx = find_index_by_symbol(update.symbol);
                if (idx >= 0)
                {
                    app_state_apply_kline_update((uint8_t)idx, &update, WS_KLINE_INTERVAL_MS);
                }
            }
            continue;
        }

        QueueSetMemberHandle_t activated = xQueueSelectFromSet(queue_set, portMAX_DELAY);
        if (updates != NULL && activated == updates)
        {
            if (xQueueReceive(updates, &update, 0) == pdTRUE)
            {
                int idx = find_index_by_symbol(update.symbol);
                if (idx >= 0)
                {
                    app_state_apply_kline_update((uint8_t)idx, &update, WS_KLINE_INTERVAL_MS);
                }
            }
        }
        else if (activated == watchlist_events)
        {
            if (xQueueReceive(watchlist_events, &watchlist_event, 0) == pdTRUE)
            {
                if (watchlist_event.kind == APP_STATE_WATCHLIST_SYMBOL_ADDED)
                {
                    if (updates == NULL)
                    {
                        updates = start_ws_client(queue_set);
                        if (updates == NULL)
                        {
                            ESP_LOGW(TAG, "Failed to start WS client for the first watchlist symbol");
                        }
                    }
                    else
                    {
                        esp_err_t err = market_data_ws_client_subscribe(watchlist_event.symbol);
                        if (err != ESP_OK)
                        {
                            ESP_LOGW(TAG, "WS subscribe failed for '%s': %s", watchlist_event.symbol,
                                     esp_err_to_name(err));
                        }
                    }
                }
                else if (watchlist_event.kind == APP_STATE_WATCHLIST_SYMBOL_REMOVED)
                {
                    if (updates != NULL)
                    {
                        esp_err_t err = market_data_ws_client_unsubscribe(watchlist_event.symbol);
                        if (err != ESP_OK)
                        {
                            ESP_LOGW(TAG, "WS unsubscribe failed for '%s': %s", watchlist_event.symbol,
                                     esp_err_to_name(err));
                        }
                        else if (app_state_symbol_count() == 0)
                        {
                            // Keep the now-idle client/queue attached to the set. The next
                            // add can SUBSCRIBE immediately, and we avoid deleting a queue
                            // that may still have an already-enqueued final tick.
                            ESP_LOGI(TAG, "Watchlist is empty; WebSocket remains idle until a symbol is added");
                        }
                    }
                }
                else // APP_STATE_REGION_CHANGED
                {
                    ESP_LOGI(TAG, "API region changed; reconnecting WS client against the new host");
                    if (updates != NULL)
                    {
                        xQueueRemoveFromSet(updates, queue_set);
                        market_data_ws_client_stop();
                        updates = NULL;
                    }
                    if (app_state_symbol_count() > 0)
                    {
                        updates = start_ws_client(queue_set);
                        if (updates == NULL)
                        {
                            ESP_LOGE(TAG, "Failed to reconnect WS client after region change");
                        }
                    }
                }
            }
        }
    }
}

esp_err_t app_state_ws_task_start(void)
{
    BaseType_t ok = xTaskCreate(ws_task_fn, "app_state_ws", WS_TASK_STACK_SIZE, NULL, WS_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
