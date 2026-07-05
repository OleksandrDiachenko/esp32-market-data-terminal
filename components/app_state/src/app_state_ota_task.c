#include "app_state_ota_task.h"

#include <stdbool.h>

#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_client.h"

#define OTA_TASK_STACK_SIZE 6144
#define OTA_TASK_PRIORITY 3
#define OTA_TASK_LOOP_INTERVAL_MS 60000        // how often the loop wakes to check the request flags
#define OTA_CHECK_INTERVAL_MS (6 * 60 * 60 * 1000) // periodic release check cadence: 6h

static const char *TAG = "app_state_ota";

static volatile bool s_check_now_requested = true; // run one check shortly after boot
static volatile bool s_update_now_requested;
static int64_t s_last_check_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Returns false on a transient failure (e.g. NOT_SYNCED because Wi-Fi/time
// aren't up yet) so the caller retries next loop iteration instead of
// waiting out the full periodic interval - same soft-dependency treatment
// as app_state_sync_task's due-fetch retries.
static bool run_check(void)
{
    ota_client_release_info_t info;
    ota_client_err_t err = ota_client_check_latest_release(&info);
    if (err != OTA_CLIENT_OK)
    {
        ESP_LOGW(TAG, "Release check skipped/failed: %d", (int)err);
        return false;
    }

    app_state_set_ota_info(info.update_available, info.tag_name);
    if (info.update_available)
    {
        ESP_LOGI(TAG, "Update available: %s", info.tag_name);
    }
    else
    {
        ESP_LOGI(TAG, "Firmware up to date (%s)", info.tag_name);
    }
    return true;
}

static void run_update(void)
{
    app_state_ota_info_t info;
    app_state_get_ota_info(&info);
    if (!info.update_available)
    {
        ESP_LOGW(TAG, "Update-now requested but no update is available; ignoring");
        return;
    }

    // Only reached on failure - ota_client_update_to() reboots the device
    // on success and does not return.
    ota_client_err_t err = ota_client_update_to(info.latest_version);
    ESP_LOGE(TAG, "Update to %s failed: %d", info.latest_version, (int)err);
}

static void ota_task_fn(void *arg)
{
    (void)arg;
    for (;;)
    {
        if (s_update_now_requested)
        {
            s_update_now_requested = false;
            run_update();
        }
        else if (s_check_now_requested || (now_ms() - s_last_check_ms >= OTA_CHECK_INTERVAL_MS))
        {
            if (run_check())
            {
                s_check_now_requested = false;
                s_last_check_ms = now_ms();
            }
            else
            {
                // Transient failure (e.g. Wi-Fi/time_sync not up yet) -
                // keep retrying every loop iteration
                // (OTA_TASK_LOOP_INTERVAL_MS) instead of waiting out the
                // full periodic interval; s_last_check_ms is only advanced
                // on success.
                s_check_now_requested = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_TASK_LOOP_INTERVAL_MS));
    }
}

esp_err_t app_state_ota_task_start(void)
{
    BaseType_t ok = xTaskCreate(ota_task_fn, "app_state_ota", OTA_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_state_ota_check_now(void)
{
    s_check_now_requested = true;
}

void app_state_ota_update_now(void)
{
    s_update_now_requested = true;
}
