#include "app_state_sync_task.h"

#include <stdbool.h>
#include <string.h>

#include "app_state.h"
#include "app_state_retry_policy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "market_data_client.h"
#include "time_sync.h"
#include "wifi_manager.h"

#define SYNC_TASK_STACK_SIZE 6144
#define SYNC_TASK_PRIORITY 4
#define SYNC_LOOP_INTERVAL_MS 2000
#define RETRY_BASE_DELAY_MS 2000
#define RETRY_MAX_DELAY_MS 60000
#define RESYNC_GAP_MS (5 * 60 * 1000) // one 5m candle interval

static const char *TAG = "app_state_sync";

static int64_t s_next_attempt_ms[APP_STATE_MAX_SYMBOLS];
static int64_t s_disconnected_since_ms; // 0 => not currently in a disconnected span
static bool s_force_resync_all;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void handle_wifi_event(const wifi_manager_event_t *evt)
{
    switch (evt->id)
    {
    case WIFI_MANAGER_EVENT_CONNECTED:
        if (s_disconnected_since_ms != 0)
        {
            int64_t offline_ms = now_ms() - s_disconnected_since_ms;
            if (app_state_retry_needs_resync((uint32_t)offline_ms, RESYNC_GAP_MS))
            {
                ESP_LOGW(TAG, "Reconnected after %lld ms offline (>= %d ms); forcing full resync",
                         (long long)offline_ms, RESYNC_GAP_MS);
                s_force_resync_all = true;
            }
            else
            {
                ESP_LOGI(TAG, "Reconnected after %lld ms offline; resuming normal cadence", (long long)offline_ms);
            }
            s_disconnected_since_ms = 0;
        }
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        if (s_disconnected_since_ms == 0)
        {
            s_disconnected_since_ms = now_ms();
        }
        break;
    default:
        break;
    }
}

// Builds the list of watchlist indices due for a fetch this cycle: never
// synced, a recoverable failure whose backoff has elapsed, or every
// symbol when a post-reconnect resync was forced. Symbols in
// APP_STATE_SYMBOL_ERROR (unrecoverable) are never included here - only a
// watchlist change (out of scope for this task) can move them forward.
static uint8_t collect_due_indices(uint8_t count, uint8_t *out_indices, bool forced)
{
    uint8_t n = 0;
    int64_t t = now_ms();
    for (uint8_t i = 0; i < count; i++)
    {
        app_state_symbol_meta_t meta;
        if (app_state_get_symbol_meta(i, &meta) != ESP_OK)
        {
            continue;
        }

        bool due = false;
        if (forced)
        {
            due = (meta.state != APP_STATE_SYMBOL_ERROR);
        }
        else if (meta.state == APP_STATE_SYMBOL_INIT)
        {
            due = true;
        }
        else if (meta.state == APP_STATE_SYMBOL_DEGRADED)
        {
            due = (t >= s_next_attempt_ms[i]);
        }

        if (due)
        {
            out_indices[n++] = i;
        }
    }
    return n;
}

static void run_due_fetches(uint8_t count, market_data_kline_t *const *scratch)
{
    if (count == 0 || !time_sync_is_synced())
    {
        return;
    }

    // Snapshot-and-clear right away, not after the (possibly slow, blocking)
    // batch HTTP call below - a new force-resync request that arrives while
    // this fetch is still in flight must not be silently dropped by an
    // unconditional clear once this fetch finishes (found during Phase 13's
    // hardware validation: two region switches in quick succession lost the
    // second one this way) - see
    // docs/decisions/0009-regional-server-auto-selection.md.
    bool forced = s_force_resync_all;
    s_force_resync_all = false;

    uint8_t due_indices[APP_STATE_MAX_SYMBOLS];
    uint8_t due_count = collect_due_indices(count, due_indices, forced);
    if (due_count == 0)
    {
        return;
    }

    const char *symbols[APP_STATE_MAX_SYMBOLS];
    market_data_kline_t *out_klines[APP_STATE_MAX_SYMBOLS];
    char symbol_storage[APP_STATE_MAX_SYMBOLS][SETTINGS_SYMBOL_MAX_LEN + 1];

    for (uint8_t i = 0; i < due_count; i++)
    {
        app_state_symbol_meta_t meta;
        app_state_get_symbol_meta(due_indices[i], &meta);
        strncpy(symbol_storage[i], meta.symbol, sizeof(symbol_storage[i]));
        symbols[i] = symbol_storage[i];
        out_klines[i] = scratch[due_indices[i]];
    }

    market_data_batch_result_t results[APP_STATE_MAX_SYMBOLS];
    market_data_err_t err = market_data_client_fetch_klines_24h_5m_batch(symbols, due_count, out_klines,
                                                                          APP_STATE_KLINE_CAPACITY, results);
    if (err != MARKET_DATA_OK)
    {
        // NOT_SYNCED (race with the check above) or bad args - nothing to
        // record, every symbol stays exactly as it was and gets retried
        // next cycle. Restore a consumed force-resync request so a forced
        // full resync isn't silently downgraded to a plain per-symbol retry.
        if (forced)
        {
            s_force_resync_all = true;
        }
        ESP_LOGW(TAG, "Batch fetch did not run: %d", (int)err);
        return;
    }

    for (uint8_t i = 0; i < due_count; i++)
    {
        uint8_t idx = due_indices[i];
        if (results[i].err == MARKET_DATA_OK)
        {
            app_state_record_success(idx, out_klines[i], results[i].count, now_ms());
            ESP_LOGI(TAG, "Synced '%s': %u candles", symbols[i], (unsigned)results[i].count);
        }
        else
        {
            bool recoverable;
            if (results[i].err == MARKET_DATA_ERR_SYMBOL_NOT_FOUND)
            {
                app_state_symbol_meta_t pre;
                app_state_get_symbol_meta(idx, &pre); // invalid_symbol_count BEFORE this failure is recorded
                recoverable = app_state_retry_invalid_symbol_is_recoverable(pre.invalid_symbol_count);
            }
            else
            {
                recoverable = app_state_retry_is_recoverable(results[i].err);
            }
            app_state_record_error(idx, results[i].err, recoverable);

            app_state_symbol_meta_t meta;
            app_state_get_symbol_meta(idx, &meta);
            if (recoverable)
            {
                uint32_t delay = app_state_retry_backoff_delay_ms(RETRY_BASE_DELAY_MS, RETRY_MAX_DELAY_MS,
                                                                    meta.retry_attempt);
                s_next_attempt_ms[idx] = now_ms() + delay;
                ESP_LOGW(TAG, "'%s' fetch failed (err=%d), retrying in %u ms", symbols[i], (int)results[i].err,
                          (unsigned)delay);
            }
            else if (results[i].err == MARKET_DATA_ERR_SYMBOL_NOT_FOUND)
            {
                ESP_LOGW(TAG,
                         "'%s' rejected as invalid symbol %u times; marking unsupported, stopping retries this "
                         "session",
                         symbols[i], (unsigned)meta.invalid_symbol_count);
            }
            else
            {
                ESP_LOGE(TAG, "'%s' fetch failed with unrecoverable err=%d; not retrying automatically", symbols[i],
                          (int)results[i].err);
            }
        }
    }
}

static void sync_task_fn(void *arg)
{
    (void)arg;

    // Sized for APP_STATE_MAX_SYMBOLS (not the boot-time watchlist size) so
    // a symbol added at runtime always has a scratch slot ready - see
    // docs/decisions/0007-watchlist-management.md's PSRAM budget note
    // (~25KB/slot against this board's 32MB PSRAM, negligible even at the
    // full 10-slot allocation).
    static market_data_kline_t *scratch[APP_STATE_MAX_SYMBOLS];
    for (uint8_t i = 0; i < APP_STATE_MAX_SYMBOLS; i++)
    {
        // Separate from app_state's own PSRAM storage: app_state only
        // exposes mutex-guarded copy accessors, not a raw writable
        // pointer, so every write goes through
        // app_state_record_success()/_error() instead of two writers
        // touching the same memory.
        scratch[i] = heap_caps_malloc(sizeof(market_data_kline_t) * APP_STATE_KLINE_CAPACITY, MALLOC_CAP_SPIRAM);
        if (scratch[i] == NULL)
        {
            ESP_LOGE(TAG, "PSRAM allocation failed for sync scratch buffer %u; sync task exiting", (unsigned)i);
            vTaskDelete(NULL);
            return;
        }
    }

    // Sole consumer of this queue by design - wifi_manager_event_t is a
    // point-to-point FreeRTOS queue, not a broadcast, so no other module
    // may also read from it without stealing events from this task.
    QueueHandle_t wifi_events = wifi_manager_get_event_queue();

    for (;;)
    {
        if (wifi_events != NULL)
        {
            wifi_manager_event_t evt;
            while (xQueueReceive(wifi_events, &evt, 0) == pdTRUE)
            {
                handle_wifi_event(&evt);
            }
        }

        // Re-read every cycle - the watchlist can grow at runtime via
        // app_state_add_symbol(), and a stale count here was the actual
        // cause of newly-added symbols never leaving APP_STATE_SYMBOL_INIT.
        run_due_fetches(app_state_symbol_count(), scratch);

        vTaskDelay(pdMS_TO_TICKS(SYNC_LOOP_INTERVAL_MS));
    }
}

esp_err_t app_state_sync_task_start(void)
{
    BaseType_t ok = xTaskCreate(sync_task_fn, "app_state_sync", SYNC_TASK_STACK_SIZE, NULL, SYNC_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_state_sync_task_force_resync(void)
{
    s_force_resync_all = true;
}
