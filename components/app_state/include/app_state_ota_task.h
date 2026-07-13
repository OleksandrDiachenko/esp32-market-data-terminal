#pragma once

// Orchestrates Phase 10's background OTA check: periodically calls
// ota_client_check_latest_release() and records the result via
// app_state_set_ota_info(). Soft dependency like app_state_sync_task -
// Wi-Fi/time_sync not being ready just means the check is skipped and
// retried next cycle, same treatment as every other network-dependent
// module in this project.
//
// Manual "check now"/"install now" actions (Settings > Updates screen) call
// ota_client_check_latest_release()/ota_client_update_to() directly instead
// of going through this task - see main/display_ui.c - since this task's
// loop only wakes every 60s and a button tap needs an immediate result.

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Creates the background OTA task. Call once, after app_state_init().
    // Runs an initial check shortly after creation, then every 6h.
    esp_err_t app_state_ota_task_start(void);

#ifdef __cplusplus
}
#endif
