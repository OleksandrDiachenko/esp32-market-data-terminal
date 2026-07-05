#include "startup_diagnostics.h"
#include "app_lifecycle.h"
#include "app_state.h"
#include "app_state_ota_task.h"
#include "app_state_sync_task.h"
#include "app_state_ws_task.h"
#include "display_ui.h"
#include "ota_console.h"
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

    // Live WebSocket updates are best-effort like the sync task itself -
    // failure here only costs live data between REST syncs.
    if (app_state_ws_task_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Market data WebSocket task failed to start; continuing without live updates");
    }

    // OTA background check is best-effort too - a failure here only means
    // no "update available" notice until the device restarts.
    if (app_state_ota_task_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA background task failed to start; continuing without update checks");
    }

    // CLI/log-driven manual OTA trigger (Phase 10's roadmap scope) - a
    // failure here is non-fatal, same as the other soft dependencies above.
    if (ota_console_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA console failed to start; manual ota_check/ota_update commands unavailable");
    }
}
