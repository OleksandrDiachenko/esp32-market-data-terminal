#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_SSID_MAX 32
#define WIFI_MANAGER_PASSWORD_MAX 64
#define WIFI_MANAGER_MAX_PROFILES 12
#define WIFI_MANAGER_MAX_SCAN_APS 20

typedef enum
{
    WIFI_MANAGER_STATE_STOPPED = 0,
    WIFI_MANAGER_STATE_STARTING,
    WIFI_MANAGER_STATE_READY,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_ERROR,
} wifi_manager_state_t;

typedef enum
{
    WIFI_MANAGER_EVENT_STARTED = 0,
    WIFI_MANAGER_EVENT_START_FAILED,
    WIFI_MANAGER_EVENT_READY_NO_PROFILES,
    WIFI_MANAGER_EVENT_SCAN_DONE,
    WIFI_MANAGER_EVENT_SCAN_FAILED,
    WIFI_MANAGER_EVENT_CONNECTING,
    WIFI_MANAGER_EVENT_CONNECTED,
    WIFI_MANAGER_EVENT_DISCONNECTED,
    WIFI_MANAGER_EVENT_AUTH_FAILED,
    WIFI_MANAGER_EVENT_CONNECT_FAILED,
    WIFI_MANAGER_EVENT_FALLBACK_STARTED,
    WIFI_MANAGER_EVENT_CYCLE_RESTARTED,
    WIFI_MANAGER_EVENT_ALL_PROFILES_BLOCKED,
} wifi_manager_event_id_t;

typedef struct
{
    wifi_manager_event_id_t id;
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
} wifi_manager_event_t;

typedef struct
{
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    bool blocked;
    bool connected;
} wifi_manager_known_t;

typedef struct
{
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    int8_t rssi;
    bool saved;
    bool connected;
} wifi_manager_ap_t;

typedef struct
{
    wifi_manager_state_t state;
    bool available;
    bool profile_storage_available;
    char active_ssid[WIFI_MANAGER_SSID_MAX + 1];
    uint8_t profile_count;
    wifi_manager_known_t known[WIFI_MANAGER_MAX_PROFILES];
    uint8_t ap_count;
    wifi_manager_ap_t aps[WIFI_MANAGER_MAX_SCAN_APS];
} wifi_manager_snapshot_t;

/**
 * Creates the manager's task, queues, and timers. Does not touch the radio
 * or the ESP-Hosted link. Call once, before wifi_manager_start().
 */
esp_err_t wifi_manager_init(void);

/**
 * Brings up the ESP-Hosted link to the ESP32-C6, starts Wi-Fi station
 * mode, and kicks off autoconnect against saved profiles. On failure to
 * bring up the radio, logs the reason and returns an error; the caller
 * must treat this as non-fatal and keep the rest of the application
 * running. Once the radio is up, all further connection management is
 * asynchronous - watch wifi_manager_get_event_queue() or poll
 * wifi_manager_get_snapshot().
 */
esp_err_t wifi_manager_start(void);

/**
 * Starts an async scan. Valid in any state once the radio is up,
 * including while already connected. Result arrives as
 * WIFI_MANAGER_EVENT_SCAN_DONE / WIFI_MANAGER_EVENT_SCAN_FAILED.
 */
esp_err_t wifi_manager_scan_async(void);

/**
 * Connects to a network not necessarily already saved. If currently
 * connected elsewhere, that network is remembered and automatically
 * retried if this attempt fails (fallback). On success, this network is
 * saved as a profile.
 */
esp_err_t wifi_manager_connect_new(const char *ssid, const char *password);

/** Connects to an already-saved profile, unblocking it first if needed. */
esp_err_t wifi_manager_connect_saved(const char *ssid);

/** Disconnects and pauses autoconnect until the next connect command. */
esp_err_t wifi_manager_disconnect(void);

/** Removes a saved profile, disconnecting first if it's the active one. */
esp_err_t wifi_manager_forget(const char *ssid);

/** Updates a saved profile's password and unblocks it. */
esp_err_t wifi_manager_update_password(const char *ssid, const char *password);

esp_err_t wifi_manager_get_snapshot(wifi_manager_snapshot_t *out_snapshot);

/** Queue of wifi_manager_event_t, owned by the manager. Do not delete it. */
QueueHandle_t wifi_manager_get_event_queue(void);

#ifdef __cplusplus
}
#endif
