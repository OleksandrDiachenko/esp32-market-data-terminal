#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_policy.h"
#include "wifi_profile_store.h"

static const char *TAG = "wifi_manager";

enum
{
    // ESP-Hosted SDIO mempool sizing (see docs/decisions/0001-wifi-connectivity.md).
    HOSTED_SDIO_MIN_MEMPOOL_BLOCKS = 11,
    HOSTED_SDIO_BUFFER_SIZE = 1536,
    HOSTED_SDIO_BLOCK_OVERHEAD = 16,

    COPROCESSOR_VERSION_RETRIES = 10,
    COPROCESSOR_VERSION_RETRY_DELAY_MS = 200,

    SCAN_MAX_APS_FETCH = 32,

    WIFI_MGR_CMD_QUEUE_LEN = 8,
    WIFI_MGR_EVENT_QUEUE_LEN = 16,
    WIFI_MGR_TASK_STACK_SIZE = 6144,
    WIFI_MGR_TASK_PRIORITY = 4,

    CONNECT_TIMEOUT_MS = 20000,
    SCAN_TIMEOUT_MS = 15000,
    RETRY_BASE_DELAY_MS = 1000,
    RETRY_MAX_DELAY_MS = 8000,
    MAX_ATTEMPTS_PER_PROFILE = 3,
    AUTH_BLOCK_THRESHOLD = 2,
    INTER_CYCLE_DELAY_MS = 200,
};

typedef enum
{
    CMD_STARTED = 0,
    CMD_WIFI_DISCONNECTED,
    CMD_IP_GOT_IP,
    CMD_SCAN_NEW,
    CMD_SCAN_DONE,
    CMD_SCAN_TIMEOUT,
    CMD_CONNECT_TIMEOUT,
    CMD_RETRY_TIMER_EXPIRED,
    CMD_CONNECT_NEW,
    CMD_CONNECT_SAVED,
    CMD_DISCONNECT,
    CMD_FORGET,
    CMD_UPDATE_PASSWORD,
} wifi_mgr_cmd_kind_t;

typedef struct
{
    wifi_mgr_cmd_kind_t kind;
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    char password[WIFI_MANAGER_PASSWORD_MAX + 1];
    uint8_t disconnect_reason;
} wifi_mgr_cmd_t;

static QueueHandle_t cmd_queue;
static QueueHandle_t event_queue;
static SemaphoreHandle_t snapshot_mutex;
static TaskHandle_t wifi_mgr_task_handle;

static esp_timer_handle_t connect_timeout_timer;
static esp_timer_handle_t retry_timer;
static esp_timer_handle_t scan_timeout_timer;

static wifi_policy_t policy;
static wifi_profile_db_t profile_db;
static bool profile_storage_available;

// Password for an SSID being connected via wifi_manager_connect_new(),
// stashed here because the network isn't saved to profile_db unless the
// connection succeeds - see docs/decisions/0001-wifi-connectivity.md.
static char pending_new_ssid[WIFI_MANAGER_SSID_MAX + 1];
static char pending_new_password[WIFI_MANAGER_PASSWORD_MAX + 1];

static wifi_manager_snapshot_t snapshot;

// Scratch buffers for scan processing, kept off the task stack.
static wifi_ap_record_t scan_records[SCAN_MAX_APS_FETCH];
static wifi_policy_scan_ap_t scan_policy_aps[SCAN_MAX_APS_FETCH];

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void connect_timeout_cb(void *arg);
static void retry_timer_cb(void *arg);
static void scan_timeout_cb(void *arg);
static void wifi_mgr_task(void *arg);
static void handle_cmd(const wifi_mgr_cmd_t *cmd);
static void feed_policy(const wifi_policy_input_t *in);
static void execute_action(const wifi_policy_action_t *action);
static void execute_connect(const char *ssid, wifi_policy_origin_t origin);
static void sync_policy_profiles_from_db(void);
static void inject_dev_profile_if_configured(void);
static bool find_password_for_ssid(const char *ssid, char *out_password, size_t out_size);
static void mark_last_success_and_save(const char *ssid);
static void set_profile_blocked_and_save(const char *ssid, bool blocked);
static void remove_profile_and_save(const char *ssid);
static void update_snapshot_from_policy(void);
static void publish_event(wifi_manager_event_id_t id, const char *ssid);
static wifi_manager_event_id_t map_policy_event(wifi_policy_event_t event);
static wifi_manager_state_t map_policy_state(wifi_policy_state_t state);
static esp_err_t enqueue_cmd(const wifi_mgr_cmd_t *cmd);

static void start_connect_timeout_timer(void)
{
    esp_timer_stop(connect_timeout_timer);
    esp_timer_start_once(connect_timeout_timer, (uint64_t)CONNECT_TIMEOUT_MS * 1000ULL);
}

static void stop_connect_timeout_timer(void)
{
    esp_timer_stop(connect_timeout_timer);
}

static void start_retry_timer(uint32_t delay_ms)
{
    esp_timer_stop(retry_timer);
    esp_timer_start_once(retry_timer, (uint64_t)delay_ms * 1000ULL);
}

static void stop_retry_timer(void)
{
    esp_timer_stop(retry_timer);
}

static void start_scan_timeout_timer(void)
{
    esp_timer_stop(scan_timeout_timer);
    esp_timer_start_once(scan_timeout_timer, (uint64_t)SCAN_TIMEOUT_MS * 1000ULL);
}

static void stop_scan_timeout_timer(void)
{
    esp_timer_stop(scan_timeout_timer);
}

static size_t hosted_sdio_mempool_required_bytes(void)
{
#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && defined(CONFIG_ESP_HOSTED_USE_MEMPOOL) && \
    CONFIG_ESP_HOSTED_USE_MEMPOOL
    return ((size_t)CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE + HOSTED_SDIO_MIN_MEMPOOL_BLOCKS) *
           (HOSTED_SDIO_BUFFER_SIZE + HOSTED_SDIO_BLOCK_OVERHEAD);
#else
    return 0;
#endif
}

static esp_err_t validate_hosted_start_memory_budget(void)
{
    const size_t required = hosted_sdio_mempool_required_bytes();
    if (required == 0U)
    {
        return ESP_OK;
    }

    const size_t internal_dma_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    const size_t spiram_dma_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "ESP-Hosted SDIO mempool budget: required=%u internal_dma_largest=%u spiram_dma_largest=%u",
             (unsigned)required, (unsigned)internal_dma_largest, (unsigned)spiram_dma_largest);

    return internal_dma_largest >= required ? ESP_OK : ESP_ERR_NO_MEM;
}

static void log_coprocessor_version(void)
{
    esp_hosted_coprocessor_fwver_t version = {0};
    for (int attempt = 0; attempt < COPROCESSOR_VERSION_RETRIES; attempt++)
    {
        if (esp_hosted_get_coprocessor_fwversion(&version) == ESP_OK)
        {
            ESP_LOGI(TAG, "ESP32-C6 co-processor firmware: %u.%u.%u", (unsigned)version.major1,
                     (unsigned)version.minor1, (unsigned)version.patch1);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(COPROCESSOR_VERSION_RETRY_DELAY_MS));
    }
    ESP_LOGW(TAG, "Could not read ESP32-C6 co-processor firmware version after %d attempts",
             COPROCESSOR_VERSION_RETRIES);
}

// --- policy <-> profile store glue -----------------------------------

static void sync_policy_profiles_from_db(void)
{
    wifi_policy_profile_t profiles[WIFI_PROFILE_MAX_PROFILES];
    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        strncpy(profiles[i].ssid, profile_db.records[i].ssid, WIFI_POLICY_SSID_MAX);
        profiles[i].ssid[WIFI_POLICY_SSID_MAX] = '\0';
        profiles[i].blocked = (profile_db.records[i].flags & WIFI_PROFILE_FLAG_BLOCKED) != 0;
    }
    wifi_policy_set_profiles(&policy, profiles, profile_db.count, profile_db.last_success_ssid);
}

static void inject_dev_profile_if_configured(void)
{
    if (CONFIG_WIFI_MANAGER_DEV_SSID[0] == '\0')
    {
        return;
    }

    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        if (strcmp(profile_db.records[i].ssid, CONFIG_WIFI_MANAGER_DEV_SSID) == 0)
        {
            return; // already saved
        }
    }

    if (profile_db.count >= WIFI_PROFILE_MAX_PROFILES)
    {
        ESP_LOGW(TAG, "Profile store full; cannot inject dev Wi-Fi profile");
        return;
    }

    uint8_t idx = profile_db.count++;
    memset(&profile_db.records[idx], 0, sizeof(profile_db.records[idx]));
    strncpy(profile_db.records[idx].ssid, CONFIG_WIFI_MANAGER_DEV_SSID, WIFI_PROFILE_SSID_MAX);
    strncpy(profile_db.records[idx].password, CONFIG_WIFI_MANAGER_DEV_PASSWORD, WIFI_PROFILE_PASSWORD_MAX);
    ESP_LOGI(TAG, "Injected dev Wi-Fi profile (Kconfig-configured SSID)");

    if (profile_storage_available)
    {
        wifi_profile_store_save(&profile_db);
    }
}

static bool find_password_for_ssid(const char *ssid, char *out_password, size_t out_size)
{
    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        if (strcmp(profile_db.records[i].ssid, ssid) == 0)
        {
            strncpy(out_password, profile_db.records[i].password, out_size - 1);
            out_password[out_size - 1] = '\0';
            return true;
        }
    }
    return false;
}

static void mark_last_success_and_save(const char *ssid)
{
    int16_t idx = -1;
    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        if (strcmp(profile_db.records[i].ssid, ssid) == 0)
        {
            idx = (int16_t)i;
            break;
        }
    }

    if (idx < 0)
    {
        if (profile_db.count >= WIFI_PROFILE_MAX_PROFILES)
        {
            ESP_LOGW(TAG, "Profile store full; cannot save network '%s'", ssid);
        }
        else
        {
            idx = (int16_t)profile_db.count++;
            memset(&profile_db.records[idx], 0, sizeof(profile_db.records[idx]));
            strncpy(profile_db.records[idx].ssid, ssid, WIFI_PROFILE_SSID_MAX);
        }
    }

    if (idx >= 0)
    {
        if (pending_new_ssid[0] != '\0' && strcmp(pending_new_ssid, ssid) == 0)
        {
            strncpy(profile_db.records[idx].password, pending_new_password, WIFI_PROFILE_PASSWORD_MAX);
        }
        profile_db.records[idx].flags &= (uint8_t)~WIFI_PROFILE_FLAG_BLOCKED;
    }

    strncpy(profile_db.last_success_ssid, ssid, WIFI_PROFILE_SSID_MAX);

    memset(pending_new_ssid, 0, sizeof(pending_new_ssid));
    memset(pending_new_password, 0, sizeof(pending_new_password));

    if (profile_storage_available)
    {
        wifi_profile_store_save(&profile_db);
    }
    sync_policy_profiles_from_db();
}

static void set_profile_blocked_and_save(const char *ssid, bool blocked)
{
    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        if (strcmp(profile_db.records[i].ssid, ssid) == 0)
        {
            if (blocked)
            {
                profile_db.records[i].flags |= WIFI_PROFILE_FLAG_BLOCKED;
            }
            else
            {
                profile_db.records[i].flags &= (uint8_t)~WIFI_PROFILE_FLAG_BLOCKED;
            }
            break;
        }
    }
    if (profile_storage_available)
    {
        wifi_profile_store_save(&profile_db);
    }
    // Deliberately not re-syncing policy from profile_db here: this action
    // fires mid-autoconnect-cycle, and wifi_policy_set_profiles() would
    // reset the cycle's cursor/remaining-count bookkeeping. Policy already
    // updated its own in-memory blocked flag as part of emitting this
    // action.
}

static void remove_profile_and_save(const char *ssid)
{
    int16_t idx = -1;
    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        if (strcmp(profile_db.records[i].ssid, ssid) == 0)
        {
            idx = (int16_t)i;
            break;
        }
    }
    if (idx < 0)
    {
        return;
    }

    for (uint8_t i = (uint8_t)idx; i < profile_db.count - 1U; i++)
    {
        profile_db.records[i] = profile_db.records[i + 1];
    }
    profile_db.count--;
    memset(&profile_db.records[profile_db.count], 0, sizeof(profile_db.records[0]));

    if (strcmp(profile_db.last_success_ssid, ssid) == 0)
    {
        profile_db.last_success_ssid[0] = '\0';
    }

    if (profile_storage_available)
    {
        wifi_profile_store_save(&profile_db);
    }
    sync_policy_profiles_from_db();
}

// --- policy action execution -------------------------------------------

static wifi_manager_event_id_t map_policy_event(wifi_policy_event_t event)
{
    switch (event)
    {
    case WIFI_POLICY_EVENT_READY_NO_PROFILES:
        return WIFI_MANAGER_EVENT_READY_NO_PROFILES;
    case WIFI_POLICY_EVENT_CONNECTING:
        return WIFI_MANAGER_EVENT_CONNECTING;
    case WIFI_POLICY_EVENT_CONNECTED:
        return WIFI_MANAGER_EVENT_CONNECTED;
    case WIFI_POLICY_EVENT_DISCONNECTED:
        return WIFI_MANAGER_EVENT_DISCONNECTED;
    case WIFI_POLICY_EVENT_AUTH_FAILED:
        return WIFI_MANAGER_EVENT_AUTH_FAILED;
    case WIFI_POLICY_EVENT_CONNECT_FAILED:
        return WIFI_MANAGER_EVENT_CONNECT_FAILED;
    case WIFI_POLICY_EVENT_FALLBACK_STARTED:
        return WIFI_MANAGER_EVENT_FALLBACK_STARTED;
    case WIFI_POLICY_EVENT_CYCLE_RESTARTED:
        return WIFI_MANAGER_EVENT_CYCLE_RESTARTED;
    case WIFI_POLICY_EVENT_ALL_PROFILES_BLOCKED:
        return WIFI_MANAGER_EVENT_ALL_PROFILES_BLOCKED;
    default:
        return WIFI_MANAGER_EVENT_CONNECT_FAILED;
    }
}

static wifi_manager_state_t map_policy_state(wifi_policy_state_t state)
{
    switch (state)
    {
    case WIFI_POLICY_STATE_READY:
        return WIFI_MANAGER_STATE_READY;
    case WIFI_POLICY_STATE_CONNECTING:
        return WIFI_MANAGER_STATE_CONNECTING;
    case WIFI_POLICY_STATE_CONNECTED:
        return WIFI_MANAGER_STATE_CONNECTED;
    case WIFI_POLICY_STATE_IDLE:
    default:
        return WIFI_MANAGER_STATE_STOPPED;
    }
}

static void publish_event(wifi_manager_event_id_t id, const char *ssid)
{
    if (event_queue == NULL)
    {
        return;
    }
    wifi_manager_event_t evt = {.id = id};
    if (ssid != NULL)
    {
        strncpy(evt.ssid, ssid, WIFI_MANAGER_SSID_MAX);
    }
    if (xQueueSend(event_queue, &evt, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Event queue full; dropping event %d", (int)id);
    }
}

static void execute_connect(const char *ssid, wifi_policy_origin_t origin)
{
    char password[WIFI_MANAGER_PASSWORD_MAX + 1] = {0};
    bool have_password = false;

    if (origin == WIFI_POLICY_ORIGIN_MANUAL && pending_new_ssid[0] != '\0' && strcmp(pending_new_ssid, ssid) == 0)
    {
        strncpy(password, pending_new_password, sizeof(password) - 1);
        have_password = pending_new_password[0] != '\0';
    }
    else
    {
        have_password = find_password_for_ssid(ssid, password, sizeof(password));
    }

    if (!have_password)
    {
        ESP_LOGW(TAG, "No stored password for '%s'; attempting open connection", ssid);
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (have_password)
    {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = have_password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    memset(password, 0, sizeof(password));

    esp_wifi_disconnect(); // best-effort clear any prior association

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    memset(&wifi_config, 0, sizeof(wifi_config));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed for '%s': %s", ssid, esp_err_to_name(ret));
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_OTHER};
        feed_policy(&in);
        return;
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_connect failed for '%s': %s", ssid, esp_err_to_name(ret));
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_OTHER};
        feed_policy(&in);
        return;
    }

    start_connect_timeout_timer();
    ESP_LOGI(TAG, "Connecting to '%s' (origin=%d)", ssid, (int)origin);
}

static void execute_action(const wifi_policy_action_t *action)
{
    switch (action->kind)
    {
    case WIFI_POLICY_ACT_CONNECT:
        execute_connect(action->ssid, action->origin);
        break;
    case WIFI_POLICY_ACT_DISCONNECT:
        stop_connect_timeout_timer();
        stop_retry_timer();
        esp_wifi_disconnect();
        break;
    case WIFI_POLICY_ACT_START_RETRY_TIMER:
        stop_connect_timeout_timer();
        start_retry_timer(action->delay_ms);
        break;
    case WIFI_POLICY_ACT_EMIT_EVENT:
        publish_event(map_policy_event(action->event), action->ssid);
        break;
    case WIFI_POLICY_ACT_MARK_LAST_SUCCESS:
        mark_last_success_and_save(action->ssid);
        break;
    case WIFI_POLICY_ACT_SET_BLOCKED:
        set_profile_blocked_and_save(action->ssid, action->flag);
        break;
    default:
        break;
    }
}

static void feed_policy(const wifi_policy_input_t *in)
{
    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    uint8_t n = wifi_policy_handle(&policy, in, actions, WIFI_POLICY_MAX_ACTIONS);
    for (uint8_t i = 0; i < n; i++)
    {
        execute_action(&actions[i]);
    }
}

static void update_snapshot_from_policy(void)
{
    if (snapshot_mutex == NULL || xSemaphoreTake(snapshot_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    snapshot.state = map_policy_state(wifi_policy_state(&policy));
    strncpy(snapshot.active_ssid, policy.current_ssid, WIFI_MANAGER_SSID_MAX);
    snapshot.active_ssid[WIFI_MANAGER_SSID_MAX] = '\0';

    snapshot.profile_count = profile_db.count;
    bool is_connected = wifi_policy_state(&policy) == WIFI_POLICY_STATE_CONNECTED;
    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        strncpy(snapshot.known[i].ssid, profile_db.records[i].ssid, WIFI_MANAGER_SSID_MAX);
        snapshot.known[i].ssid[WIFI_MANAGER_SSID_MAX] = '\0';
        snapshot.known[i].blocked = (profile_db.records[i].flags & WIFI_PROFILE_FLAG_BLOCKED) != 0;
        snapshot.known[i].connected = is_connected && strcmp(profile_db.records[i].ssid, policy.current_ssid) == 0;
    }

    xSemaphoreGive(snapshot_mutex);
}

// --- command handling ----------------------------------------------------

static void handle_scan_new(void)
{
    esp_err_t ret = esp_wifi_scan_start(NULL, false);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        publish_event(WIFI_MANAGER_EVENT_SCAN_FAILED, NULL);
        return;
    }
    start_scan_timeout_timer();
}

static void handle_scan_done(void)
{
    stop_scan_timeout_timer();

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    uint16_t fetched = ap_count > SCAN_MAX_APS_FETCH ? SCAN_MAX_APS_FETCH : ap_count;

    esp_err_t ret = esp_wifi_scan_get_ap_records(&fetched, scan_records);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(ret));
        publish_event(WIFI_MANAGER_EVENT_SCAN_FAILED, NULL);
        return;
    }

    for (uint16_t i = 0; i < fetched; i++)
    {
        strncpy(scan_policy_aps[i].ssid, (const char *)scan_records[i].ssid, WIFI_POLICY_SSID_MAX);
        scan_policy_aps[i].ssid[WIFI_POLICY_SSID_MAX] = '\0';
        scan_policy_aps[i].rssi = scan_records[i].rssi;
    }

    wifi_policy_profile_t profiles[WIFI_PROFILE_MAX_PROFILES];
    for (uint8_t i = 0; i < profile_db.count; i++)
    {
        strncpy(profiles[i].ssid, profile_db.records[i].ssid, WIFI_POLICY_SSID_MAX);
        profiles[i].ssid[WIFI_POLICY_SSID_MAX] = '\0';
        profiles[i].blocked = (profile_db.records[i].flags & WIFI_PROFILE_FLAG_BLOCKED) != 0;
    }

    uint8_t sorted_count = wifi_policy_sort_scan(scan_policy_aps, (uint8_t)fetched, WIFI_MANAGER_MAX_SCAN_APS,
                                                  profiles, profile_db.count, policy.current_ssid);

    if (snapshot_mutex != NULL && xSemaphoreTake(snapshot_mutex, portMAX_DELAY) == pdTRUE)
    {
        snapshot.ap_count = sorted_count;
        for (uint8_t i = 0; i < sorted_count; i++)
        {
            strncpy(snapshot.aps[i].ssid, scan_policy_aps[i].ssid, WIFI_MANAGER_SSID_MAX);
            snapshot.aps[i].ssid[WIFI_MANAGER_SSID_MAX] = '\0';
            snapshot.aps[i].rssi = scan_policy_aps[i].rssi;
            snapshot.aps[i].saved = scan_policy_aps[i].saved;
            snapshot.aps[i].connected = scan_policy_aps[i].connected;
        }
        xSemaphoreGive(snapshot_mutex);
    }

    publish_event(WIFI_MANAGER_EVENT_SCAN_DONE, NULL);
}

static void handle_started(void)
{
    bool store_ok = false;
    wifi_profile_store_init(&store_ok);
    profile_storage_available = store_ok;
    if (snapshot_mutex != NULL && xSemaphoreTake(snapshot_mutex, portMAX_DELAY) == pdTRUE)
    {
        snapshot.profile_storage_available = store_ok;
        xSemaphoreGive(snapshot_mutex);
    }
    ESP_LOGI(TAG, "Wi-Fi profile storage available: %s", store_ok ? "yes" : "no");

    wifi_profile_store_load(&profile_db);
    inject_dev_profile_if_configured();
    sync_policy_profiles_from_db();

    wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_STARTED};
    feed_policy(&in);
}

static void handle_cmd(const wifi_mgr_cmd_t *cmd)
{
    switch (cmd->kind)
    {
    case CMD_STARTED:
        handle_started();
        break;

    case CMD_WIFI_DISCONNECTED:
    {
        wifi_policy_state_t pstate = wifi_policy_state(&policy);
        if (pstate == WIFI_POLICY_STATE_CONNECTING)
        {
            stop_connect_timeout_timer();
            wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CONNECT_FAIL,
                                       .fail_class = wifi_policy_classify_reason(cmd->disconnect_reason)};
            feed_policy(&in);
        }
        else if (pstate == WIFI_POLICY_STATE_CONNECTED)
        {
            wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_LINK_LOST};
            feed_policy(&in);
        }
        break;
    }

    case CMD_IP_GOT_IP:
    {
        stop_connect_timeout_timer();
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
        feed_policy(&in);
        break;
    }

    case CMD_SCAN_NEW:
        handle_scan_new();
        break;

    case CMD_SCAN_DONE:
        handle_scan_done();
        break;

    case CMD_SCAN_TIMEOUT:
        publish_event(WIFI_MANAGER_EVENT_SCAN_FAILED, NULL);
        break;

    case CMD_CONNECT_TIMEOUT:
    {
        esp_wifi_disconnect();
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CONNECT_TIMEOUT};
        feed_policy(&in);
        break;
    }

    case CMD_RETRY_TIMER_EXPIRED:
    {
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_RETRY_TIMER_EXPIRED};
        feed_policy(&in);
        break;
    }

    case CMD_CONNECT_NEW:
    {
        strncpy(pending_new_ssid, cmd->ssid, WIFI_MANAGER_SSID_MAX);
        strncpy(pending_new_password, cmd->password, WIFI_MANAGER_PASSWORD_MAX);
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
        strncpy(in.ssid, cmd->ssid, WIFI_POLICY_SSID_MAX);
        feed_policy(&in);
        break;
    }

    case CMD_CONNECT_SAVED:
    {
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CMD_CONNECT_SAVED};
        strncpy(in.ssid, cmd->ssid, WIFI_POLICY_SSID_MAX);
        feed_policy(&in);
        break;
    }

    case CMD_DISCONNECT:
    {
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CMD_DISCONNECT};
        feed_policy(&in);
        break;
    }

    case CMD_FORGET:
    {
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CMD_FORGET};
        strncpy(in.ssid, cmd->ssid, WIFI_POLICY_SSID_MAX);
        feed_policy(&in);
        remove_profile_and_save(cmd->ssid);
        break;
    }

    case CMD_UPDATE_PASSWORD:
    {
        for (uint8_t i = 0; i < profile_db.count; i++)
        {
            if (strcmp(profile_db.records[i].ssid, cmd->ssid) == 0)
            {
                strncpy(profile_db.records[i].password, cmd->password, WIFI_PROFILE_PASSWORD_MAX);
                break;
            }
        }
        if (profile_storage_available)
        {
            wifi_profile_store_save(&profile_db);
        }
        wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_CMD_UPDATE_PASSWORD};
        strncpy(in.ssid, cmd->ssid, WIFI_POLICY_SSID_MAX);
        feed_policy(&in);
        break;
    }

    default:
        break;
    }

    update_snapshot_from_policy();
}

static void wifi_mgr_task(void *arg)
{
    (void)arg;
    wifi_mgr_cmd_t cmd;
    for (;;)
    {
        if (xQueueReceive(cmd_queue, &cmd, portMAX_DELAY) == pdTRUE)
        {
            handle_cmd(&cmd);
        }
    }
}

// --- event handlers / timer callbacks (ISR-safe: only enqueue) -----------

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT)
    {
        return;
    }

    wifi_mgr_cmd_t cmd = {0};
    if (event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        cmd.kind = CMD_WIFI_DISCONNECTED;
        const wifi_event_sta_disconnected_t *data = (const wifi_event_sta_disconnected_t *)event_data;
        cmd.disconnect_reason = (data != NULL) ? data->reason : 0;
    }
    else if (event_id == WIFI_EVENT_SCAN_DONE)
    {
        cmd.kind = CMD_SCAN_DONE;
    }
    else
    {
        return;
    }
    xQueueSend(cmd_queue, &cmd, 0);
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP)
    {
        return;
    }
    wifi_mgr_cmd_t cmd = {.kind = CMD_IP_GOT_IP};
    xQueueSend(cmd_queue, &cmd, 0);
}

static void connect_timeout_cb(void *arg)
{
    (void)arg;
    wifi_mgr_cmd_t cmd = {.kind = CMD_CONNECT_TIMEOUT};
    xQueueSend(cmd_queue, &cmd, 0);
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    wifi_mgr_cmd_t cmd = {.kind = CMD_RETRY_TIMER_EXPIRED};
    xQueueSend(cmd_queue, &cmd, 0);
}

static void scan_timeout_cb(void *arg)
{
    (void)arg;
    wifi_mgr_cmd_t cmd = {.kind = CMD_SCAN_TIMEOUT};
    xQueueSend(cmd_queue, &cmd, 0);
}

static esp_err_t enqueue_cmd(const wifi_mgr_cmd_t *cmd)
{
    if (cmd_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(cmd_queue, cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

// --- public API ------------------------------------------------------------

esp_err_t wifi_manager_init(void)
{
    if (cmd_queue != NULL)
    {
        return ESP_OK;
    }

    cmd_queue = xQueueCreate(WIFI_MGR_CMD_QUEUE_LEN, sizeof(wifi_mgr_cmd_t));
    event_queue = xQueueCreate(WIFI_MGR_EVENT_QUEUE_LEN, sizeof(wifi_manager_event_t));
    snapshot_mutex = xSemaphoreCreateMutex();
    if (cmd_queue == NULL || event_queue == NULL || snapshot_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t connect_timer_args = {.callback = connect_timeout_cb, .name = "wifi_connect_to"};
    const esp_timer_create_args_t retry_timer_args = {.callback = retry_timer_cb, .name = "wifi_retry"};
    const esp_timer_create_args_t scan_timer_args = {.callback = scan_timeout_cb, .name = "wifi_scan_to"};
    if (esp_timer_create(&connect_timer_args, &connect_timeout_timer) != ESP_OK ||
        esp_timer_create(&retry_timer_args, &retry_timer) != ESP_OK ||
        esp_timer_create(&scan_timer_args, &scan_timeout_timer) != ESP_OK)
    {
        return ESP_FAIL;
    }

    const wifi_policy_config_t policy_cfg = {
        .retry_base_delay_ms = RETRY_BASE_DELAY_MS,
        .retry_max_delay_ms = RETRY_MAX_DELAY_MS,
        .max_attempts_per_profile = MAX_ATTEMPTS_PER_PROFILE,
        .auth_block_threshold = AUTH_BLOCK_THRESHOLD,
        .inter_cycle_delay_ms = INTER_CYCLE_DELAY_MS,
    };
    wifi_policy_init(&policy, &policy_cfg);

    snapshot.state = WIFI_MANAGER_STATE_STOPPED;
    snapshot.available = false;

    if (xTaskCreate(wifi_mgr_task, "wifi_mgr", WIFI_MGR_TASK_STACK_SIZE, NULL, WIFI_MGR_TASK_PRIORITY,
                     &wifi_mgr_task_handle) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (snapshot.state == WIFI_MANAGER_STATE_READY || snapshot.state == WIFI_MANAGER_STATE_CONNECTING ||
        snapshot.state == WIFI_MANAGER_STATE_CONNECTED)
    {
        return ESP_OK;
    }

    snapshot.state = WIFI_MANAGER_STATE_STARTING;
    esp_err_t ret = ESP_OK;

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        goto err;
    }

    if (esp_netif_create_default_wifi_sta() == NULL)
    {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        ret = ESP_FAIL;
        goto err;
    }

    ret = validate_hosted_start_memory_budget();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Not enough internal DMA memory to start ESP-Hosted Wi-Fi link");
        goto err;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        goto err;
    }

    // This component owns credentials; do not let IDF persist them to the
    // default NVS partition.
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(ret));
        goto err;
    }
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(ret));
        goto err;
    }

    snapshot.available = true;
    ESP_LOGI(TAG, "Wi-Fi station started over ESP-Hosted link");

    log_coprocessor_version();

    wifi_mgr_cmd_t cmd = {.kind = CMD_STARTED};
    if (enqueue_cmd(&cmd) != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not enqueue startup command");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    return ESP_OK;

err:
    snapshot.state = WIFI_MANAGER_STATE_ERROR;
    snapshot.available = false;
    return ret;
}

esp_err_t wifi_manager_scan_async(void)
{
    wifi_mgr_cmd_t cmd = {.kind = CMD_SCAN_NEW};
    return enqueue_cmd(&cmd);
}

esp_err_t wifi_manager_connect_new(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_MANAGER_SSID_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > WIFI_MANAGER_PASSWORD_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_mgr_cmd_t cmd = {.kind = CMD_CONNECT_NEW};
    strncpy(cmd.ssid, ssid, WIFI_MANAGER_SSID_MAX);
    if (password != NULL)
    {
        strncpy(cmd.password, password, WIFI_MANAGER_PASSWORD_MAX);
    }
    esp_err_t ret = enqueue_cmd(&cmd);
    memset(cmd.password, 0, sizeof(cmd.password));
    return ret;
}

esp_err_t wifi_manager_connect_saved(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_MANAGER_SSID_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_mgr_cmd_t cmd = {.kind = CMD_CONNECT_SAVED};
    strncpy(cmd.ssid, ssid, WIFI_MANAGER_SSID_MAX);
    return enqueue_cmd(&cmd);
}

esp_err_t wifi_manager_disconnect(void)
{
    wifi_mgr_cmd_t cmd = {.kind = CMD_DISCONNECT};
    return enqueue_cmd(&cmd);
}

esp_err_t wifi_manager_forget(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_MANAGER_SSID_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_mgr_cmd_t cmd = {.kind = CMD_FORGET};
    strncpy(cmd.ssid, ssid, WIFI_MANAGER_SSID_MAX);
    return enqueue_cmd(&cmd);
}

esp_err_t wifi_manager_update_password(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_MANAGER_SSID_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > WIFI_MANAGER_PASSWORD_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_mgr_cmd_t cmd = {.kind = CMD_UPDATE_PASSWORD};
    strncpy(cmd.ssid, ssid, WIFI_MANAGER_SSID_MAX);
    if (password != NULL)
    {
        strncpy(cmd.password, password, WIFI_MANAGER_PASSWORD_MAX);
    }
    esp_err_t ret = enqueue_cmd(&cmd);
    memset(cmd.password, 0, sizeof(cmd.password));
    return ret;
}

esp_err_t wifi_manager_get_snapshot(wifi_manager_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (snapshot_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(snapshot_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    *out_snapshot = snapshot;
    xSemaphoreGive(snapshot_mutex);
    return ESP_OK;
}

QueueHandle_t wifi_manager_get_event_queue(void)
{
    return event_queue;
}
