#include "app_state.h"

#include <string.h>

#include "app_state_kline_merge.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings_store.h"

static const char *TAG = "app_state";

typedef struct
{
    char symbol[SETTINGS_SYMBOL_MAX_LEN + 1];
    app_state_symbol_state_t state;
    market_data_err_t last_error;
    uint8_t retry_attempt;
    int64_t last_sync_time_ms;
    uint16_t kline_count;
    market_data_kline_t *klines; // PSRAM, APP_STATE_KLINE_CAPACITY entries
} symbol_slot_t;

static SemaphoreHandle_t s_lock;
static symbol_slot_t s_symbols[APP_STATE_MAX_SYMBOLS];
static uint8_t s_symbol_count;
static app_state_ota_info_t s_ota_info;
static QueueHandle_t s_watchlist_event_queue;

static void push_watchlist_event(app_state_watchlist_event_kind_t kind, const char *symbol)
{
    if (s_watchlist_event_queue == NULL)
    {
        return;
    }
    app_state_watchlist_event_t evt = {.kind = kind};
    strncpy(evt.symbol, symbol, SETTINGS_SYMBOL_MAX_LEN);
    evt.symbol[SETTINGS_SYMBOL_MAX_LEN] = '\0';
    if (xQueueSend(s_watchlist_event_queue, &evt, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Watchlist event queue full; dropping event for '%s'", symbol);
    }
}

QueueHandle_t app_state_get_watchlist_event_queue(void)
{
    return s_watchlist_event_queue;
}

esp_err_t app_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_watchlist_event_queue =
        xQueueCreate(APP_STATE_WATCHLIST_EVENT_QUEUE_LEN, sizeof(app_state_watchlist_event_t));
    if (s_watchlist_event_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    symbol_settings_t cfg;
    settings_store_load_symbols(&cfg); // always ESP_OK, defaults (empty watchlist) substituted on corruption

    s_symbol_count = (cfg.count > APP_STATE_MAX_SYMBOLS) ? APP_STATE_MAX_SYMBOLS : cfg.count;
    if (s_symbol_count == 0)
    {
        ESP_LOGW(TAG, "Watchlist is empty; nothing to sync until symbols are configured");
        return ESP_OK;
    }

    for (uint8_t i = 0; i < s_symbol_count; i++)
    {
        symbol_slot_t *slot = &s_symbols[i];
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->symbol, cfg.symbols[i].ticker, SETTINGS_SYMBOL_MAX_LEN);
        slot->state = APP_STATE_SYMBOL_INIT;

        slot->klines = heap_caps_malloc(sizeof(market_data_kline_t) * APP_STATE_KLINE_CAPACITY, MALLOC_CAP_SPIRAM);
        if (slot->klines == NULL)
        {
            ESP_LOGE(TAG, "PSRAM allocation failed for '%s' (%u bytes)", slot->symbol,
                     (unsigned)(sizeof(market_data_kline_t) * APP_STATE_KLINE_CAPACITY));
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Loaded %u watchlist symbol(s)", (unsigned)s_symbol_count);
    return ESP_OK;
}

uint8_t app_state_symbol_count(void)
{
    return s_symbol_count;
}

esp_err_t app_state_get_symbol_meta(uint8_t index, app_state_symbol_meta_t *out_meta)
{
    if (out_meta == NULL || index >= s_symbol_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    symbol_slot_t *slot = &s_symbols[index];
    strncpy(out_meta->symbol, slot->symbol, SETTINGS_SYMBOL_MAX_LEN);
    out_meta->symbol[SETTINGS_SYMBOL_MAX_LEN] = '\0';
    out_meta->state = slot->state;
    out_meta->last_error = slot->last_error;
    out_meta->retry_attempt = slot->retry_attempt;
    out_meta->last_sync_time_ms = slot->last_sync_time_ms;
    out_meta->kline_count = slot->kline_count;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t app_state_get_symbol_klines(uint8_t index, market_data_kline_t *out_klines, uint16_t out_capacity,
                                       uint16_t *out_count)
{
    if (out_klines == NULL || out_count == NULL || index >= s_symbol_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    symbol_slot_t *slot = &s_symbols[index];
    uint16_t count = (slot->kline_count < out_capacity) ? slot->kline_count : out_capacity;
    memcpy(out_klines, slot->klines, count * sizeof(market_data_kline_t));
    *out_count = count;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t app_state_add_symbol(const char *ticker)
{
    if (ticker == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_symbol_count >= APP_STATE_MAX_SYMBOLS)
    {
        return ESP_ERR_NO_MEM;
    }

    // Allocate before taking the lock - PSRAM allocation shouldn't happen
    // while other tasks are blocked waiting on it for unrelated accessors.
    market_data_kline_t *klines = heap_caps_malloc(sizeof(market_data_kline_t) * APP_STATE_KLINE_CAPACITY, MALLOC_CAP_SPIRAM);
    if (klines == NULL)
    {
        ESP_LOGE(TAG, "PSRAM allocation failed for '%s' (%u bytes)", ticker,
                 (unsigned)(sizeof(market_data_kline_t) * APP_STATE_KLINE_CAPACITY));
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_symbol_count >= APP_STATE_MAX_SYMBOLS) // re-check under the lock against a concurrent add
    {
        xSemaphoreGive(s_lock);
        heap_caps_free(klines);
        return ESP_ERR_NO_MEM;
    }
    symbol_slot_t *slot = &s_symbols[s_symbol_count];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->symbol, ticker, SETTINGS_SYMBOL_MAX_LEN);
    slot->state = APP_STATE_SYMBOL_INIT;
    slot->klines = klines;
    s_symbol_count++;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Added watchlist symbol '%s' (%u total)", slot->symbol, (unsigned)s_symbol_count);
    push_watchlist_event(APP_STATE_WATCHLIST_SYMBOL_ADDED, ticker);
    return ESP_OK;
}

esp_err_t app_state_remove_symbol(uint8_t index)
{
    if (index >= s_symbol_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index >= s_symbol_count) // re-check under the lock against a concurrent remove
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    market_data_kline_t *klines_to_free = s_symbols[index].klines;
    char removed_symbol[SETTINGS_SYMBOL_MAX_LEN + 1];
    strncpy(removed_symbol, s_symbols[index].symbol, sizeof(removed_symbol));
    for (uint8_t i = index; i < s_symbol_count - 1; i++)
    {
        s_symbols[i] = s_symbols[i + 1];
    }
    s_symbol_count--;
    xSemaphoreGive(s_lock);

    heap_caps_free(klines_to_free); // freeing PSRAM doesn't need the lock held
    ESP_LOGI(TAG, "Removed watchlist symbol '%s' (%u remaining)", removed_symbol, (unsigned)s_symbol_count);
    push_watchlist_event(APP_STATE_WATCHLIST_SYMBOL_REMOVED, removed_symbol);
    return ESP_OK;
}

esp_err_t app_state_move_symbol(uint8_t from_index, uint8_t to_index)
{
    if (from_index >= s_symbol_count || to_index >= s_symbol_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (from_index >= s_symbol_count || to_index >= s_symbol_count) // re-check under the lock against a concurrent mutation
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (from_index == to_index)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    symbol_slot_t moved = s_symbols[from_index];
    if (from_index < to_index)
    {
        for (uint8_t i = from_index; i < to_index; i++)
        {
            s_symbols[i] = s_symbols[i + 1];
        }
    }
    else
    {
        for (uint8_t i = from_index; i > to_index; i--)
        {
            s_symbols[i] = s_symbols[i - 1];
        }
    }
    s_symbols[to_index] = moved;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Moved watchlist symbol '%s' from %u to %u", moved.symbol, (unsigned)from_index, (unsigned)to_index);
    return ESP_OK;
}

esp_err_t app_state_record_success(uint8_t index, const market_data_kline_t *klines, uint16_t count, int64_t now_ms)
{
    if (klines == NULL || index >= s_symbol_count || count > APP_STATE_KLINE_CAPACITY)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    symbol_slot_t *slot = &s_symbols[index];
    memcpy(slot->klines, klines, count * sizeof(market_data_kline_t));
    slot->kline_count = count;
    slot->state = APP_STATE_SYMBOL_SYNCED;
    slot->last_error = MARKET_DATA_OK;
    slot->retry_attempt = 0;
    slot->last_sync_time_ms = now_ms;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t app_state_record_error(uint8_t index, market_data_err_t err, bool recoverable)
{
    if (index >= s_symbol_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    symbol_slot_t *slot = &s_symbols[index];
    slot->last_error = err;
    if (recoverable)
    {
        slot->state = APP_STATE_SYMBOL_DEGRADED;
        if (slot->retry_attempt < UINT8_MAX)
        {
            slot->retry_attempt++;
        }
    }
    else
    {
        slot->state = APP_STATE_SYMBOL_ERROR;
        slot->retry_attempt = 0;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t app_state_apply_kline_update(uint8_t index, const market_data_kline_update_t *update, int64_t interval_ms)
{
    if (update == NULL || index >= s_symbol_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    symbol_slot_t *slot = &s_symbols[index];
    app_state_kline_merge_apply(slot->klines, &slot->kline_count, APP_STATE_KLINE_CAPACITY, interval_ms, update);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t app_state_get_ota_info(app_state_ota_info_t *out_info)
{
    if (out_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lock == NULL)
    {
        // Called before app_state_init() - display_ui_start() creates the
        // 1s status-bar timer before app_state_init() runs, and that timer
        // reads OTA info on every tick. Same all-zero "never checked" state
        // as post-init but before the first check, not an error.
        *out_info = (app_state_ota_info_t){0};
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_info = s_ota_info;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t app_state_set_ota_info(bool update_available, const char *latest_version)
{
    if (update_available && latest_version == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ota_info.update_available = update_available;
    if (latest_version != NULL)
    {
        strncpy(s_ota_info.latest_version, latest_version, APP_STATE_OTA_VERSION_MAX_LEN - 1);
        s_ota_info.latest_version[APP_STATE_OTA_VERSION_MAX_LEN - 1] = '\0';
    }
    else
    {
        s_ota_info.latest_version[0] = '\0';
    }
    s_ota_info.last_check_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
