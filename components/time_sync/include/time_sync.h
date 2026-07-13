#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Applies the persisted timezone (settings_store's locale_settings_t) and
     * arms SNTP sync to run once the station gets an IP. Safe to call before
     * Wi-Fi is up: registers on IP_EVENT_STA_GOT_IP and does the actual NTP
     * exchange in a background task, so this never blocks the caller. Time
     * sync is a soft dependency like Wi-Fi itself - failure is logged and
     * always returns ESP_OK.
     */
    esp_err_t time_sync_start(void);

    /** True once an SNTP sync has completed successfully since boot. */
    bool time_sync_is_synced(void);

#ifdef __cplusplus
}
#endif
