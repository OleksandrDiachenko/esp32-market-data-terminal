#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wifi_profile_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the encrypted NVS-backed Wi-Fi profile store. On any failure
// (missing nvs_keys partition, NVS encryption disabled, corrupt partition
// even after one erase-and-retry), logs the reason, sets *out_available to
// false, and returns ESP_OK — a missing profile store must not prevent
// Wi-Fi from starting, only saved-profile persistence is unavailable.
esp_err_t wifi_profile_store_init(bool *out_available);

// Loads the profile database. If the store is unavailable or empty/corrupt,
// fills *out_db with an empty, validly-sealed database and returns ESP_OK.
esp_err_t wifi_profile_store_load(wifi_profile_db_t *out_db);

// Seals (stamps + CRCs) and persists db. Returns an error if the store is
// unavailable; the caller keeps running with in-memory-only profiles.
esp_err_t wifi_profile_store_save(wifi_profile_db_t *db);

#ifdef __cplusplus
}
#endif
