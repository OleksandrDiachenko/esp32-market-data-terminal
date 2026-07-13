#pragma once

#include "esp_err.h"
#include "settings_codec.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Loads display settings from the "settings" namespace of the default NVS
    // partition. Always returns ESP_OK; if nothing is stored yet, or the
    // stored blob is corrupt/outdated, *out is filled with sane defaults.
    esp_err_t settings_store_load_display(display_settings_t *out);

    // Seals (stamps + CRCs) and persists cfg.
    esp_err_t settings_store_save_display(display_settings_t *cfg);

    esp_err_t settings_store_load_symbols(symbol_settings_t *out);
    esp_err_t settings_store_save_symbols(symbol_settings_t *cfg);

    esp_err_t settings_store_load_locale(locale_settings_t *out);
    esp_err_t settings_store_save_locale(locale_settings_t *cfg);

    esp_err_t settings_store_load_api_region(api_region_settings_t *out);
    esp_err_t settings_store_save_api_region(api_region_settings_t *cfg);

    esp_err_t settings_store_load_disclaimer(disclaimer_settings_t *out);
    esp_err_t settings_store_save_disclaimer(disclaimer_settings_t *cfg);

#ifdef __cplusplus
}
#endif
