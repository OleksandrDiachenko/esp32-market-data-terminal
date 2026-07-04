#include "app_state.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
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

esp_err_t app_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
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
