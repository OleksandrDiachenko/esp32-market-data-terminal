#include "app_lifecycle.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app_lifecycle";

static esp_err_t init_default_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Default NVS partition needs erase (%s), retrying", esp_err_to_name(ret));
        esp_err_t erase_ret = nvs_flash_erase();
        if (erase_ret != ESP_OK)
        {
            return erase_ret;
        }
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t app_lifecycle_start(void)
{
    ESP_LOGI(TAG, "Starting application lifecycle...");

    esp_err_t ret = init_default_nvs();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Default NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Application lifecycle started.");
    return ESP_OK;
}