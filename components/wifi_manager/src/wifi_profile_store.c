#include "wifi_profile_store.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_profile_store";

#define WIFI_CFG_PARTITION "wifi_cfg"
#define NVS_KEYS_PARTITION "nvs_keys"
#define NVS_NAMESPACE "wifi"
#define NVS_PROFILES_KEY "profiles"

static bool store_available;

esp_err_t wifi_profile_store_init(bool *out_available)
{
    if (out_available == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    store_available = false;
    *out_available = false;

#if !CONFIG_NVS_ENCRYPTION
    ESP_LOGW(TAG, "Wi-Fi profile store unavailable: CONFIG_NVS_ENCRYPTION is disabled");
    return ESP_OK;
#else
    const esp_partition_t *key_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NVS_KEYS_PARTITION);
    if (key_partition == NULL)
    {
        ESP_LOGW(TAG, "Wi-Fi profile store unavailable: no '%s' partition", NVS_KEYS_PARTITION);
        return ESP_OK;
    }

    nvs_sec_cfg_t sec_cfg = {0};
    esp_err_t err = nvs_flash_read_security_cfg(key_partition, &sec_cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED)
    {
        ESP_LOGI(TAG, "Generating encrypted NVS keys for Wi-Fi profiles");
        err = nvs_flash_generate_keys(key_partition, &sec_cfg);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi profile store unavailable: security config failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    err = nvs_flash_secure_init_partition(WIFI_CFG_PARTITION, &sec_cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Wi-Fi profile partition needs erase (%s), retrying", esp_err_to_name(err));
        err = nvs_flash_erase_partition(WIFI_CFG_PARTITION);
        if (err == ESP_OK)
        {
            err = nvs_flash_secure_init_partition(WIFI_CFG_PARTITION, &sec_cfg);
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi profile store unavailable: partition init failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    store_available = true;
    *out_available = true;
    ESP_LOGI(TAG, "Wi-Fi profile store ready (encrypted)");
    return ESP_OK;
#endif
}

esp_err_t wifi_profile_store_load(wifi_profile_db_t *out_db)
{
    if (out_db == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Always start from a valid, empty, sealed db so callers get a usable
    // value even when the store is unavailable or has nothing saved yet.
    wifi_profile_codec_init_empty(out_db);
    wifi_profile_codec_seal(out_db);

    if (!store_available)
    {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(WIFI_CFG_PARTITION, NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not open Wi-Fi profile store: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    wifi_profile_db_t loaded;
    size_t length = sizeof(loaded);
    err = nvs_get_blob(handle, NVS_PROFILES_KEY, &loaded, &length);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (err != ESP_OK || length != sizeof(loaded))
    {
        ESP_LOGW(TAG, "Could not read Wi-Fi profile blob: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    if (wifi_profile_codec_validate(&loaded) != WIFI_PROFILE_CODEC_OK)
    {
        ESP_LOGW(TAG, "Stored Wi-Fi profile blob failed validation; starting empty");
        return ESP_OK;
    }

    *out_db = loaded;
    return ESP_OK;
}

esp_err_t wifi_profile_store_save(wifi_profile_db_t *db)
{
    if (db == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_available)
    {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_profile_codec_seal(db);

    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(WIFI_CFG_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not open Wi-Fi profile store for write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_PROFILES_KEY, db, sizeof(*db));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not persist Wi-Fi profiles: %s", esp_err_to_name(err));
    }
    return err;
}
