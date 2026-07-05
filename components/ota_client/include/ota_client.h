#pragma once

// OTA firmware update via this repo's public GitHub Releases - see
// docs/decisions/0006-ota-firmware-update.md. No API keys/auth (public
// repo, public release assets). This component has no _init()/_start():
// every call here is a plain blocking function invoked from the caller's
// own task, same shape as market_data_client. Periodic polling and a
// manual trigger are Phase 10's next delivery slice, not this one's.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    OTA_CLIENT_OK = 0,
    OTA_CLIENT_ERR_INVALID_ARG,
    OTA_CLIENT_ERR_NOT_SYNCED, // time_sync_is_synced() == false; call not attempted
    OTA_CLIENT_ERR_NETWORK,    // connect/TLS/socket failure
    OTA_CLIENT_ERR_TIMEOUT,
    OTA_CLIENT_ERR_HTTP_STATUS, // non-2xx from the GitHub API
    OTA_CLIENT_ERR_PARSE,       // "tag_name" not found within the read cap
    OTA_CLIENT_ERR_NO_MEM,
    OTA_CLIENT_ERR_OTA_FAILED, // esp_https_ota download/flash/validate failed
} ota_client_err_t;

#define OTA_CLIENT_TAG_MAX_LEN 32

typedef struct
{
    char tag_name[OTA_CLIENT_TAG_MAX_LEN]; // e.g. "0.10.0"
    bool update_available;                 // tag_name != the running esp_app_desc_t.version
} ota_client_release_info_t;

// Fetches GET /repos/<owner>/<repo>/releases/latest from the GitHub API,
// extracts the "tag_name" field, and compares it (exact string match, no
// semver ordering - see the ADR) against the running firmware's
// esp_app_desc_t.version.
ota_client_err_t ota_client_check_latest_release(ota_client_release_info_t *out_info);

// Downloads and flashes the firmware asset for release `tag` via
// esp_https_ota, then reboots on success (does not return). `tag` is
// normally an ota_client_release_info_t.tag_name from a prior
// ota_client_check_latest_release() call that reported update_available.
// On failure, returns without having touched the currently-running app
// partition - esp_https_ota writes to the other OTA slot.
ota_client_err_t ota_client_update_to(const char *tag);

#ifdef __cplusplus
}
#endif
