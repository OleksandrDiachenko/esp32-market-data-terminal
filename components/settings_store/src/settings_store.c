#include "settings_store.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings_store";

// Unlike wifi_profile_store, this data is not secret, so it lives in the
// default NVS partition (already initialized by app_lifecycle_start())
// instead of a dedicated encrypted partition.
#define NVS_NAMESPACE "settings"
#define NVS_KEY_DISPLAY "display"
#define NVS_KEY_SYMBOLS "symbols"
#define NVS_KEY_LOCALE "locale"
#define NVS_KEY_API_REGION "api_region"
#define NVS_KEY_DISCLAIMER "disclaimer"

static esp_err_t load_blob(const char *key, void *out, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    size_t length = size;
    err = nvs_get_blob(handle, key, out, &length);
    nvs_close(handle);

    if (err != ESP_OK)
    {
        return err;
    }
    if (length != size)
    {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    return ESP_OK;
}

static esp_err_t save_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not open settings namespace for write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not persist settings blob '%s': %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_store_load_display(display_settings_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    display_settings_t loaded;
    esp_err_t err = load_blob(NVS_KEY_DISPLAY, &loaded, sizeof(loaded));
    if (err == ESP_OK && settings_display_validate(&loaded) == SETTINGS_CODEC_OK)
    {
        *out = loaded;
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Could not read display settings, using defaults: %s", esp_err_to_name(err));
    }
    settings_display_init_default(out);
    return ESP_OK;
}

esp_err_t settings_store_save_display(display_settings_t *cfg)
{
    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    settings_display_seal(cfg);
    return save_blob(NVS_KEY_DISPLAY, cfg, sizeof(*cfg));
}

esp_err_t settings_store_load_symbols(symbol_settings_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    symbol_settings_t loaded;
    esp_err_t err = load_blob(NVS_KEY_SYMBOLS, &loaded, sizeof(loaded));
    if (err == ESP_OK && settings_symbols_validate(&loaded) == SETTINGS_CODEC_OK)
    {
        *out = loaded;
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Could not read symbol settings, using defaults: %s", esp_err_to_name(err));
    }
    settings_symbols_init_default(out);
    return ESP_OK;
}

esp_err_t settings_store_save_symbols(symbol_settings_t *cfg)
{
    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    settings_symbols_seal(cfg);
    return save_blob(NVS_KEY_SYMBOLS, cfg, sizeof(*cfg));
}

esp_err_t settings_store_load_locale(locale_settings_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    locale_settings_t loaded;
    esp_err_t err = load_blob(NVS_KEY_LOCALE, &loaded, sizeof(loaded));
    if (err == ESP_OK && settings_locale_validate(&loaded) == SETTINGS_CODEC_OK)
    {
        *out = loaded;
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Could not read locale settings, using defaults: %s", esp_err_to_name(err));
    }
    settings_locale_init_default(out);
    return ESP_OK;
}

esp_err_t settings_store_save_locale(locale_settings_t *cfg)
{
    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    settings_locale_seal(cfg);
    return save_blob(NVS_KEY_LOCALE, cfg, sizeof(*cfg));
}

esp_err_t settings_store_load_api_region(api_region_settings_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    api_region_settings_t loaded;
    esp_err_t err = load_blob(NVS_KEY_API_REGION, &loaded, sizeof(loaded));
    if (err == ESP_OK && settings_api_region_validate(&loaded) == SETTINGS_CODEC_OK)
    {
        *out = loaded;
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Could not read API region settings, using defaults: %s", esp_err_to_name(err));
    }
    settings_api_region_init_default(out);
    return ESP_OK;
}

esp_err_t settings_store_save_api_region(api_region_settings_t *cfg)
{
    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    settings_api_region_seal(cfg);
    return save_blob(NVS_KEY_API_REGION, cfg, sizeof(*cfg));
}

esp_err_t settings_store_load_disclaimer(disclaimer_settings_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    disclaimer_settings_t loaded;
    esp_err_t err = load_blob(NVS_KEY_DISCLAIMER, &loaded, sizeof(loaded));
    if (err == ESP_OK && settings_disclaimer_validate(&loaded) == SETTINGS_CODEC_OK)
    {
        *out = loaded;
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Could not read disclaimer settings, using defaults: %s", esp_err_to_name(err));
    }
    settings_disclaimer_init_default(out);
    return ESP_OK;
}

esp_err_t settings_store_save_disclaimer(disclaimer_settings_t *cfg)
{
    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    settings_disclaimer_seal(cfg);
    return save_blob(NVS_KEY_DISCLAIMER, cfg, sizeof(*cfg));
}
