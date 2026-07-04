#include "startup_diagnostics.h"
#include "app_lifecycle.h"
#include "app_state.h"
#include "app_state_sync_task.h"
#include "display_ui.h"
#include "time_sync.h"
#include "wifi_manager.h"

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

    // Wi-Fi is not required for the UI to be useful; a failed link must not
    // take down the rest of the application.
    if (wifi_manager_init() != ESP_OK || wifi_manager_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi failed to start; continuing without network");
    }

    // Time sync is likewise best-effort: it arms itself against Wi-Fi
    // connecting and never blocks startup.
    if (time_sync_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Time sync failed to start; continuing with unsynced clock");
    }

    // app_state_init() only touches NVS/PSRAM (no network), so unlike
    // Wi-Fi/time_sync a failure here means a genuine resource problem, not
    // "no network yet" - treated as fatal like the other core modules
    // above.
    if (app_state_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Runtime state model failed to initialize");
        return;
    }

    // The sync task itself is best-effort like Wi-Fi/time_sync: it waits
    // out both being unavailable and simply retries, so a failure to
    // create it only costs live market data, not the rest of the app.
    if (app_state_sync_task_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Market data sync task failed to start; continuing without live data");
    }
}
