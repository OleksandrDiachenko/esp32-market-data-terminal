#include "time_sync.h"

#include <stdlib.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings_store.h"

static const char *TAG = "time_sync";

#define SNTP_SERVER "pool.ntp.org"
#define SNTP_SYNC_TIMEOUT_MS 15000
#define SNTP_SYNC_TASK_STACK_SIZE 3072
#define SNTP_SYNC_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

static volatile bool synced;
static volatile bool sync_task_running;

static void apply_persisted_timezone(void)
{
    locale_settings_t locale;
    settings_store_load_locale(&locale);
    setenv("TZ", locale.posix_tz, 1);
    tzset();
}

static void sntp_sync_task(void *arg)
{
    (void)arg;

    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
    }
    else
    {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_SYNC_TIMEOUT_MS));
        if (err == ESP_OK)
        {
            synced = true;
            ESP_LOGI(TAG, "System time synced via SNTP");
        }
        else
        {
            ESP_LOGW(TAG, "SNTP sync did not complete: %s", esp_err_to_name(err));
        }
    }

    sync_task_running = false;
    vTaskDelete(NULL);
}

// Runs on the shared system event loop task, so it must not block: it only
// spawns sntp_sync_task and returns.
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP)
    {
        return;
    }
    if (sync_task_running)
    {
        return;
    }

    sync_task_running = true;
    if (xTaskCreate(sntp_sync_task, "sntp_sync", SNTP_SYNC_TASK_STACK_SIZE, NULL, SNTP_SYNC_TASK_PRIORITY, NULL) !=
        pdPASS)
    {
        ESP_LOGW(TAG, "Could not start SNTP sync task");
        sync_task_running = false;
    }
}

esp_err_t time_sync_start(void)
{
    apply_persisted_timezone();

    // Defensive: wifi_manager_start() normally creates these first, but
    // time_sync must not assume init order relative to other components.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    config.start = false; // don't touch the network before Wi-Fi is up
    config.wait_for_sync = true;

    err = esp_netif_sntp_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

bool time_sync_is_synced(void) { return synced; }
