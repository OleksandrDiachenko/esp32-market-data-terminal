#include "startup_diagnostics.h"
#include "app_lifecycle.h"
#include "display_ui.h"

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

    if (app_lifecycle_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Application lifecycle failed to start");
        return;
    }

    if (display_ui_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Display UI failed to start");
        return;
    }
}
