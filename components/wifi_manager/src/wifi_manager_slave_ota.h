#pragma once

#include "esp_err.h"

/**
 * Compares the ESP32-C6 co-processor firmware embedded in the "slave_fw"
 * data partition against what the C6 is currently running
 * (esp_hosted_get_coprocessor_fwversion()). If they differ, pushes the
 * partition's image to the C6 over the existing SDIO transport
 * (esp_hosted_slave_ota_begin/write/end/activate) and reboots the host so
 * both sides resync on the new version.
 *
 * Soft dependency: a missing/empty partition or an OTA failure is logged
 * and returns ESP_OK, matching the rest of wifi_manager's Wi-Fi bring-up -
 * a stale co-processor version must not block the app.
 */
esp_err_t wifi_manager_slave_ota_check_and_update(void);
