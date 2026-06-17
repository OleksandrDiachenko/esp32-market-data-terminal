#include "app_lifecycle.h"
#include "esp_log.h"

static const char *TAG = "app_lifecycle";

esp_err_t app_lifecycle_start(void)
{
    ESP_LOGI(TAG, "Starting application lifecycle...");
    ESP_LOGI(TAG, "Application lifecycle started.");
    return ESP_OK;
}