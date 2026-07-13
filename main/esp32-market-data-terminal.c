#include "startup_diagnostics.h"
#include "app_lifecycle.h"
#include "app_state.h"
#include "board_jc4880p443c.h"
#include "app_state_ota_task.h"
#include "app_state_sync_task.h"
#include "app_state_ws_task.h"
#include "dev_console.h"
#include "dev_screenshot_console.h"
#include "display_ui.h"
#include "time_sync.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "market_terminal";

void app_main(void)
{
    // First, before any logging or init: the backlight pin's power-on state
    // isn't guaranteed off, and display bring-up (when LEDC takes the pin
    // over, at duty 0) is ~1.5s away behind NVS/lifecycle init - an
    // uninitialized panel glowing white for that window is the last
    // remaining piece of the boot white-screen bug (see
    // docs/decisions/0012, "Amended" section).
    if (board_jc4880p443c_backlight_early_off() != ESP_OK)
    {
        ESP_LOGW(TAG, "Backlight early-off failed; a brief boot flash may be visible");
    }

    ESP_LOGI(TAG, "Market Data Ticker started");

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

    // Confirms this boot as workable now that the core (non-best-effort)
    // startup path above has run without crashing/looping - a prerequisite
    // for a newly OTA-flashed image (see docs/decisions/0006). A no-op if
    // rollback support is disabled or this boot wasn't pending verification
    // (e.g. a factory flash), so failure here is logged but never fatal.
    esp_err_t mark_valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (mark_valid_err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(mark_valid_err));
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

    // Dev-only console REPL (no-op unless CONFIG_DEV_SCREENSHOT_CONSOLE is
    // enabled locally) - the screenshot/nav commands below register into it.
    if (dev_console_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Dev console failed to start; screenshot/nav commands unavailable");
    }

    // Dev-only "screenshot" console command (no-op unless
    // CONFIG_DEV_SCREENSHOT_CONSOLE is enabled locally) - shares the same
    // console REPL dev_console_start() just registered into.
    if (dev_screenshot_console_register() != ESP_OK)
    {
        ESP_LOGW(TAG, "Dev screenshot console command failed to register");
    }

    // Dev-only "nav" console command - same gate as the screenshot command
    // above, lets tools/dev_screenshot.py --nav jump to a screen before
    // capturing it.
    if (display_ui_register_dev_nav_console() != ESP_OK)
    {
        ESP_LOGW(TAG, "Dev nav console command failed to register");
    }
}
