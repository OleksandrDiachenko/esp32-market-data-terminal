#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    WIFI_MANAGER_STATE_STOPPED = 0,
    WIFI_MANAGER_STATE_STARTING,
    WIFI_MANAGER_STATE_READY,
    WIFI_MANAGER_STATE_ERROR,
} wifi_manager_state_t;

typedef struct
{
    wifi_manager_state_t state;
    bool available;
    bool profile_storage_available;
} wifi_manager_snapshot_t;

/**
 * Prepares the manager. Does not touch the radio or the ESP-Hosted link.
 */
esp_err_t wifi_manager_init(void);

/**
 * Brings up the ESP-Hosted link to the ESP32-C6 and starts Wi-Fi station
 * mode. On failure, logs the reason and returns an error; the caller must
 * treat this as non-fatal and keep the rest of the application running.
 */
esp_err_t wifi_manager_start(void);

esp_err_t wifi_manager_get_snapshot(wifi_manager_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif
