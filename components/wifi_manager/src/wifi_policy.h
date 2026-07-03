#pragma once

// Pure C, host-compilable: no ESP-IDF includes, no dynamic allocation.
// Owns Wi-Fi connection *decisions* (state machine, autoconnect order,
// retry/backoff, auth-block, fallback). The adapter (wifi_manager.c) owns
// every FreeRTOS/esp_wifi/NVS object and executes the actions this module
// emits; it never makes decisions of its own.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_POLICY_SSID_MAX 32
#define WIFI_POLICY_MAX_PROFILES 12
#define WIFI_POLICY_MAX_SCAN_APS 20
#define WIFI_POLICY_MAX_ACTIONS 4

typedef enum
{
    WIFI_POLICY_STATE_IDLE = 0,
    WIFI_POLICY_STATE_READY,
    WIFI_POLICY_STATE_CONNECTING,
    WIFI_POLICY_STATE_CONNECTED,
} wifi_policy_state_t;

typedef enum
{
    WIFI_POLICY_ORIGIN_NONE = 0,
    WIFI_POLICY_ORIGIN_AUTOCONNECT,
    WIFI_POLICY_ORIGIN_MANUAL,
    WIFI_POLICY_ORIGIN_FALLBACK,
} wifi_policy_origin_t;

typedef enum
{
    WIFI_POLICY_FAIL_AUTH = 0,
    WIFI_POLICY_FAIL_AP_NOT_FOUND,
    WIFI_POLICY_FAIL_OTHER,
} wifi_policy_fail_class_t;

typedef enum
{
    WIFI_POLICY_IN_STARTED = 0,
    WIFI_POLICY_IN_CONNECT_SUCCESS,
    WIFI_POLICY_IN_CONNECT_FAIL,
    WIFI_POLICY_IN_CONNECT_TIMEOUT,
    WIFI_POLICY_IN_LINK_LOST,
    WIFI_POLICY_IN_RETRY_TIMER_EXPIRED,
    WIFI_POLICY_IN_CMD_CONNECT_NEW,
    WIFI_POLICY_IN_CMD_CONNECT_SAVED,
    WIFI_POLICY_IN_CMD_DISCONNECT,
    WIFI_POLICY_IN_CMD_FORGET,
    WIFI_POLICY_IN_CMD_UPDATE_PASSWORD,
} wifi_policy_input_kind_t;

typedef struct
{
    wifi_policy_input_kind_t kind;
    char ssid[WIFI_POLICY_SSID_MAX + 1]; // for CMD_* inputs
    wifi_policy_fail_class_t fail_class; // for CONNECT_FAIL
} wifi_policy_input_t;

typedef enum
{
    WIFI_POLICY_ACT_CONNECT = 0,
    WIFI_POLICY_ACT_DISCONNECT,
    WIFI_POLICY_ACT_START_RETRY_TIMER,
    WIFI_POLICY_ACT_EMIT_EVENT,
    WIFI_POLICY_ACT_MARK_LAST_SUCCESS,
    WIFI_POLICY_ACT_SET_BLOCKED,
} wifi_policy_action_kind_t;

typedef enum
{
    WIFI_POLICY_EVENT_READY_NO_PROFILES = 0,
    WIFI_POLICY_EVENT_CONNECTING,
    WIFI_POLICY_EVENT_CONNECTED,
    WIFI_POLICY_EVENT_DISCONNECTED,
    WIFI_POLICY_EVENT_AUTH_FAILED,
    WIFI_POLICY_EVENT_CONNECT_FAILED,
    WIFI_POLICY_EVENT_FALLBACK_STARTED,
    WIFI_POLICY_EVENT_CYCLE_RESTARTED,
    WIFI_POLICY_EVENT_ALL_PROFILES_BLOCKED,
} wifi_policy_event_t;

typedef struct
{
    wifi_policy_action_kind_t kind;
    char ssid[WIFI_POLICY_SSID_MAX + 1]; // CONNECT / SET_BLOCKED / MARK_LAST_SUCCESS
    uint32_t delay_ms;                   // START_RETRY_TIMER
    wifi_policy_event_t event;           // EMIT_EVENT
    wifi_policy_origin_t origin;         // CONNECT
    bool flag;                           // SET_BLOCKED (blocked value)
} wifi_policy_action_t;

typedef struct
{
    char ssid[WIFI_POLICY_SSID_MAX + 1];
    bool blocked;
} wifi_policy_profile_t;

typedef struct
{
    char ssid[WIFI_POLICY_SSID_MAX + 1];
    int8_t rssi;
    bool saved;
    bool connected;
} wifi_policy_scan_ap_t;

typedef struct
{
    uint32_t retry_base_delay_ms;
    uint32_t retry_max_delay_ms;
    uint8_t max_attempts_per_profile;
    uint8_t auth_block_threshold;
    uint32_t inter_cycle_delay_ms;
} wifi_policy_config_t;

typedef enum
{
    WIFI_POLICY_RETRY_NONE = 0,
    WIFI_POLICY_RETRY_SAME_PROFILE,
    WIFI_POLICY_RETRY_NEW_CYCLE,
} wifi_policy_retry_kind_t;

typedef struct
{
    wifi_policy_config_t config;

    wifi_policy_state_t state;
    wifi_policy_origin_t current_origin;
    char current_ssid[WIFI_POLICY_SSID_MAX + 1];
    uint8_t current_attempt;

    char fallback_ssid[WIFI_POLICY_SSID_MAX + 1];
    bool fallback_pending;

    wifi_policy_profile_t profiles[WIFI_POLICY_MAX_PROFILES];
    uint8_t profile_count;
    uint8_t order[WIFI_POLICY_MAX_PROFILES]; // indices into profiles[], last_success first
    uint8_t consecutive_auth_fails[WIFI_POLICY_MAX_PROFILES];
    char last_success_ssid[WIFI_POLICY_SSID_MAX + 1];

    uint8_t cursor;             // index into order[] of current/last autoconnect candidate
    uint8_t remaining_in_cycle; // profiles left to try before a cycle is exhausted
    wifi_policy_retry_kind_t pending_retry_kind;
} wifi_policy_t;

void wifi_policy_init(wifi_policy_t *p, const wifi_policy_config_t *config);

// Replaces the known-profile view. Rebuilds the autoconnect order
// (last_success_ssid first, then given order). Does not itself emit
// actions; call before wifi_policy_handle(IN_STARTED, ...).
void wifi_policy_set_profiles(wifi_policy_t *p, const wifi_policy_profile_t *profiles, uint8_t count,
                               const char *last_success_ssid);

// Processes one input, writes up to WIFI_POLICY_MAX_ACTIONS actions to
// out (caller-owned array), returns the number written.
uint8_t wifi_policy_handle(wifi_policy_t *p, const wifi_policy_input_t *in, wifi_policy_action_t *out,
                            uint8_t max_out);

wifi_policy_state_t wifi_policy_state(const wifi_policy_t *p);

// Standalone pure helpers, independently host-tested.
uint32_t wifi_policy_backoff_delay_ms(uint32_t base_ms, uint32_t max_ms, uint8_t attempt);
wifi_policy_fail_class_t wifi_policy_classify_reason(uint8_t reason);

// Sorts *in place* by RSSI descending, dedupes by SSID (keeps strongest),
// caps to max_out, marks saved/connected. Returns the final count.
uint8_t wifi_policy_sort_scan(wifi_policy_scan_ap_t *aps, uint8_t count, uint8_t max_out,
                               const wifi_policy_profile_t *profiles, uint8_t profile_count,
                               const char *connected_ssid);

#ifdef __cplusplus
}
#endif
