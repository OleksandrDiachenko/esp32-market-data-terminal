#pragma once

// Orchestrates Phase 10's background OTA check: periodically calls
// ota_client_check_latest_release() and records the result via
// app_state_set_ota_info(). Soft dependency like app_state_sync_task -
// Wi-Fi/time_sync not being ready just means the check is skipped and
// retried next cycle, same treatment as every other network-dependent
// module in this project.
//
// Also exposes the CLI/log-driven manual trigger Phase 10's roadmap scope
// calls for until Phase 11's Settings screen exists:
// app_state_ota_check_now() runs a check on the task's next loop iteration
// instead of waiting for the periodic interval; app_state_ota_update_now()
// flashes the release app_state_get_ota_info() last reported as available.
// Both are safe to call from any task (e.g. a console command handler).

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates the background OTA task. Call once, after app_state_init().
// Runs an initial check shortly after creation, then every 6h.
esp_err_t app_state_ota_task_start(void);

void app_state_ota_check_now(void);

void app_state_ota_update_now(void);

#ifdef __cplusplus
}
#endif
