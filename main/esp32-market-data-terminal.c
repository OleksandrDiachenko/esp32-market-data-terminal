#include "startup_diagnostics.h"

#include "esp_log.h"

static const char *TAG = "market_terminal";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Market Data Terminal started");

    if (startup_diagnostics() != ESP_OK)
    {
        ESP_LOGE(TAG, "Startup diagnostics failed");
        return;
    }
}
