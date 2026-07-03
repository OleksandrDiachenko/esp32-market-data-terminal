#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_profile_store.h"

static const char *TAG = "wifi_manager";

enum
{
    // ESP-Hosted SDIO mempool sizing (see docs/decisions/0001-wifi-connectivity.md).
    // Mirrors the formula validated on the same board/link in a sibling project.
    HOSTED_SDIO_MIN_MEMPOOL_BLOCKS = 11,
    HOSTED_SDIO_BUFFER_SIZE = 1536,
    HOSTED_SDIO_BLOCK_OVERHEAD = 16,

    COPROCESSOR_VERSION_RETRIES = 10,
    COPROCESSOR_VERSION_RETRY_DELAY_MS = 200,

    SCAN_MAX_APS = 20,
};

static wifi_manager_snapshot_t snapshot = {
    .state = WIFI_MANAGER_STATE_STOPPED,
    .available = false,
};

static size_t hosted_sdio_mempool_required_bytes(void)
{
#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && defined(CONFIG_ESP_HOSTED_USE_MEMPOOL) && \
    CONFIG_ESP_HOSTED_USE_MEMPOOL
    return ((size_t)CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE + HOSTED_SDIO_MIN_MEMPOOL_BLOCKS) *
           (HOSTED_SDIO_BUFFER_SIZE + HOSTED_SDIO_BLOCK_OVERHEAD);
#else
    return 0;
#endif
}

static esp_err_t validate_hosted_start_memory_budget(void)
{
    const size_t required = hosted_sdio_mempool_required_bytes();
    if (required == 0U)
    {
        return ESP_OK;
    }

    const size_t internal_dma_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    const size_t spiram_dma_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "ESP-Hosted SDIO mempool budget: required=%u internal_dma_largest=%u spiram_dma_largest=%u",
             (unsigned)required, (unsigned)internal_dma_largest, (unsigned)spiram_dma_largest);

    return internal_dma_largest >= required ? ESP_OK : ESP_ERR_NO_MEM;
}

static void log_coprocessor_version(void)
{
    esp_hosted_coprocessor_fwver_t version = {0};
    for (int attempt = 0; attempt < COPROCESSOR_VERSION_RETRIES; attempt++)
    {
        if (esp_hosted_get_coprocessor_fwversion(&version) == ESP_OK)
        {
            ESP_LOGI(TAG, "ESP32-C6 co-processor firmware: %u.%u.%u", (unsigned)version.major1,
                     (unsigned)version.minor1, (unsigned)version.patch1);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(COPROCESSOR_VERSION_RETRY_DELAY_MS));
    }
    // Non-fatal: a version query failure does not mean the link is unusable,
    // and pinned esp_hosted/esp_wifi_remote versions are already validated
    // against this board's C6 firmware.
    ESP_LOGW(TAG, "Could not read ESP32-C6 co-processor firmware version after %d attempts",
             COPROCESSOR_VERSION_RETRIES);
}

// One-shot bring-up smoke test: confirms the Hosted link and STA mode work
// end to end. Replaced by the full async scan API in a later phase.
static void scan_and_log(void)
{
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Startup scan failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > SCAN_MAX_APS)
    {
        ap_count = SCAN_MAX_APS;
    }

    wifi_ap_record_t records[SCAN_MAX_APS];
    uint16_t fetched = ap_count;
    ret = esp_wifi_scan_get_ap_records(&fetched, records);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not read scan results: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Startup scan found %u network(s)", (unsigned)fetched);
    for (uint16_t i = 0; i < fetched; i++)
    {
        ESP_LOGI(TAG, "  %s (rssi=%d)", (const char *)records[i].ssid, records[i].rssi);
    }
}

esp_err_t wifi_manager_init(void)
{
    snapshot.state = WIFI_MANAGER_STATE_STOPPED;
    snapshot.available = false;
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (snapshot.state == WIFI_MANAGER_STATE_READY)
    {
        return ESP_OK;
    }

    snapshot.state = WIFI_MANAGER_STATE_STARTING;
    esp_err_t ret = ESP_OK;

    // Profile store failure is non-fatal: Wi-Fi still starts, it just can't
    // remember networks across reboots (see wifi_profile_store.h).
    bool profile_storage_available = false;
    (void)wifi_profile_store_init(&profile_storage_available);
    snapshot.profile_storage_available = profile_storage_available;
    ESP_LOGI(TAG, "Wi-Fi profile storage available: %s", profile_storage_available ? "yes" : "no");

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        goto err;
    }

    if (esp_netif_create_default_wifi_sta() == NULL)
    {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        ret = ESP_FAIL;
        goto err;
    }

    ret = validate_hosted_start_memory_budget();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Not enough internal DMA memory to start ESP-Hosted Wi-Fi link");
        goto err;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        goto err;
    }

    // This component owns credentials (see wifi_profile_store in a later
    // phase); do not let IDF persist them to the default NVS partition.
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        goto err;
    }

    snapshot.state = WIFI_MANAGER_STATE_READY;
    snapshot.available = true;
    ESP_LOGI(TAG, "Wi-Fi station started over ESP-Hosted link");

    log_coprocessor_version();
    scan_and_log();

    return ESP_OK;

err:
    snapshot.state = WIFI_MANAGER_STATE_ERROR;
    snapshot.available = false;
    return ret;
}

esp_err_t wifi_manager_get_snapshot(wifi_manager_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_snapshot = snapshot;
    return ESP_OK;
}
