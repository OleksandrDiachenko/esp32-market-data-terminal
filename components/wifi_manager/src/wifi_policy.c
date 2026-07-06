#include "wifi_policy.h"

#include <string.h>

// Numeric Wi-Fi disconnect reason codes mirrored from ESP-IDF's
// wifi_err_reason_t (esp_wifi_types.h), so this file stays host-compilable.
// The adapter (wifi_manager.c) _Static_asserts these against the real IDF
// values so drift is caught at compile time, not at runtime.
#define WIFI_POLICY_REASON_AUTH_EXPIRE 2
#define WIFI_POLICY_REASON_MIC_FAILURE 14
#define WIFI_POLICY_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_POLICY_REASON_NO_AP_FOUND 201
#define WIFI_POLICY_REASON_AUTH_FAIL 202
#define WIFI_POLICY_REASON_HANDSHAKE_TIMEOUT 204

static void copy_ssid(char *dst, const char *src)
{
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i < WIFI_POLICY_SSID_MAX && src[i] != '\0'; i++)
    {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static uint8_t emit(wifi_policy_action_t *out, uint8_t max_out, uint8_t n, wifi_policy_action_t action)
{
    if (n < max_out)
    {
        out[n] = action;
        return (uint8_t)(n + 1);
    }
    return n;
}

static wifi_policy_action_t act_connect(const char *ssid, wifi_policy_origin_t origin)
{
    wifi_policy_action_t a = {0};
    a.kind = WIFI_POLICY_ACT_CONNECT;
    copy_ssid(a.ssid, ssid);
    a.origin = origin;
    return a;
}

static wifi_policy_action_t act_disconnect(void)
{
    wifi_policy_action_t a = {0};
    a.kind = WIFI_POLICY_ACT_DISCONNECT;
    return a;
}

static wifi_policy_action_t act_retry_timer(uint32_t delay_ms)
{
    wifi_policy_action_t a = {0};
    a.kind = WIFI_POLICY_ACT_START_RETRY_TIMER;
    a.delay_ms = delay_ms;
    return a;
}

static wifi_policy_action_t act_event(wifi_policy_event_t event, const char *ssid)
{
    wifi_policy_action_t a = {0};
    a.kind = WIFI_POLICY_ACT_EMIT_EVENT;
    a.event = event;
    copy_ssid(a.ssid, ssid);
    return a;
}

static wifi_policy_action_t act_mark_last_success(const char *ssid)
{
    wifi_policy_action_t a = {0};
    a.kind = WIFI_POLICY_ACT_MARK_LAST_SUCCESS;
    copy_ssid(a.ssid, ssid);
    return a;
}

static wifi_policy_action_t act_set_blocked(const char *ssid, bool blocked)
{
    wifi_policy_action_t a = {0};
    a.kind = WIFI_POLICY_ACT_SET_BLOCKED;
    copy_ssid(a.ssid, ssid);
    a.flag = blocked;
    return a;
}

void wifi_policy_init(wifi_policy_t *p, const wifi_policy_config_t *config)
{
    memset(p, 0, sizeof(*p));
    p->config = *config;
    p->state = WIFI_POLICY_STATE_IDLE;
}

void wifi_policy_set_profiles(wifi_policy_t *p, const wifi_policy_profile_t *profiles, uint8_t count,
                               const char *last_success_ssid)
{
    if (count > WIFI_POLICY_MAX_PROFILES)
    {
        count = WIFI_POLICY_MAX_PROFILES;
    }

    p->profile_count = count;
    for (uint8_t i = 0; i < count; i++)
    {
        p->profiles[i] = profiles[i];
    }
    copy_ssid(p->last_success_ssid, last_success_ssid);
    memset(p->consecutive_auth_fails, 0, sizeof(p->consecutive_auth_fails));

    int16_t last_success_idx = -1;
    if (p->last_success_ssid[0] != '\0')
    {
        for (uint8_t i = 0; i < count; i++)
        {
            if (strcmp(p->profiles[i].ssid, p->last_success_ssid) == 0)
            {
                last_success_idx = (int16_t)i;
                break;
            }
        }
    }

    uint8_t used[WIFI_POLICY_MAX_PROFILES] = {0};
    uint8_t pos = 0;
    if (last_success_idx >= 0)
    {
        p->order[pos++] = (uint8_t)last_success_idx;
        used[last_success_idx] = 1;
    }
    for (uint8_t i = 0; i < count; i++)
    {
        if (!used[i])
        {
            p->order[pos++] = i;
            used[i] = 1;
        }
    }

    p->cursor = 0;
    p->remaining_in_cycle = 0;
    p->pending_retry_kind = WIFI_POLICY_RETRY_NONE;
}

// Starts (or continues) an autoconnect cycle from order[start_cursor],
// trying up to `remaining` profiles, skipping blocked ones. Requires
// p->profile_count > 0.
static uint8_t begin_from(wifi_policy_t *p, uint8_t start_cursor, uint8_t remaining, wifi_policy_action_t *out,
                           uint8_t max_out, uint8_t n)
{
    uint8_t cursor = start_cursor;
    uint8_t left = remaining;

    while (left > 0)
    {
        uint8_t idx = p->order[cursor];
        if (!p->profiles[idx].blocked)
        {
            p->cursor = cursor;
            p->remaining_in_cycle = left;
            p->current_attempt = 0;
            p->current_origin = WIFI_POLICY_ORIGIN_AUTOCONNECT;
            copy_ssid(p->current_ssid, p->profiles[idx].ssid);
            p->state = WIFI_POLICY_STATE_CONNECTING;
            p->pending_retry_kind = WIFI_POLICY_RETRY_NONE;

            n = emit(out, max_out, n, act_connect(p->current_ssid, WIFI_POLICY_ORIGIN_AUTOCONNECT));
            n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_CONNECTING, p->current_ssid));
            return n;
        }
        cursor = (uint8_t)((cursor + 1) % p->profile_count);
        left--;
    }

    p->state = WIFI_POLICY_STATE_READY;
    p->current_origin = WIFI_POLICY_ORIGIN_NONE;
    p->current_ssid[0] = '\0';
    n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_ALL_PROFILES_BLOCKED, NULL));
    return n;
}

// Requires p->profile_count > 0 (only called from AUTOCONNECT contexts).
static uint8_t on_profile_exhausted(wifi_policy_t *p, wifi_policy_action_t *out, uint8_t max_out, uint8_t n)
{
    uint8_t next_cursor = (uint8_t)((p->cursor + 1) % p->profile_count);
    uint8_t next_remaining = (uint8_t)(p->remaining_in_cycle - 1);

    if (next_remaining == 0)
    {
        p->state = WIFI_POLICY_STATE_READY;
        p->current_origin = WIFI_POLICY_ORIGIN_NONE;
        p->pending_retry_kind = WIFI_POLICY_RETRY_NEW_CYCLE;
        n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_CYCLE_RESTARTED, NULL));
        n = emit(out, max_out, n, act_retry_timer(p->config.inter_cycle_delay_ms));
        return n;
    }

    return begin_from(p, next_cursor, next_remaining, out, max_out, n);
}

static uint8_t handle_terminal_failure(wifi_policy_t *p, wifi_policy_fail_class_t fail_class,
                                        wifi_policy_action_t *out, uint8_t max_out, uint8_t n)
{
    wifi_policy_event_t fail_event =
        (fail_class == WIFI_POLICY_FAIL_AUTH) ? WIFI_POLICY_EVENT_AUTH_FAILED : WIFI_POLICY_EVENT_CONNECT_FAILED;

    if (p->current_origin == WIFI_POLICY_ORIGIN_MANUAL || p->current_origin == WIFI_POLICY_ORIGIN_FALLBACK)
    {
        n = emit(out, max_out, n, act_event(fail_event, p->current_ssid));

        if (p->fallback_pending)
        {
            char fallback[WIFI_POLICY_SSID_MAX + 1];
            copy_ssid(fallback, p->fallback_ssid);
            p->fallback_pending = false;
            p->fallback_ssid[0] = '\0';

            p->current_origin = WIFI_POLICY_ORIGIN_FALLBACK;
            copy_ssid(p->current_ssid, fallback);
            p->current_attempt = 0;
            p->state = WIFI_POLICY_STATE_CONNECTING;

            n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_FALLBACK_STARTED, fallback));
            n = emit(out, max_out, n, act_connect(fallback, WIFI_POLICY_ORIGIN_FALLBACK));
            return n;
        }

        p->state = WIFI_POLICY_STATE_READY;
        p->current_origin = WIFI_POLICY_ORIGIN_NONE;
        p->current_ssid[0] = '\0';
        return n;
    }

    // AUTOCONNECT origin (including a lost-link retry, which IN_LINK_LOST
    // relabels as AUTOCONNECT): apply attempt budget and auth-block.
    uint8_t idx = p->order[p->cursor];

    if (fail_class == WIFI_POLICY_FAIL_AUTH)
    {
        if (p->consecutive_auth_fails[idx] < 0xFFU)
        {
            p->consecutive_auth_fails[idx]++;
        }
        if (p->consecutive_auth_fails[idx] >= p->config.auth_block_threshold)
        {
            p->profiles[idx].blocked = true;
            n = emit(out, max_out, n, act_set_blocked(p->profiles[idx].ssid, true));
            n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_AUTH_FAILED, p->current_ssid));
            return on_profile_exhausted(p, out, max_out, n);
        }
    }
    else
    {
        p->consecutive_auth_fails[idx] = 0;
    }

    n = emit(out, max_out, n, act_event(fail_event, p->current_ssid));

    p->current_attempt++;
    if (p->current_attempt < p->config.max_attempts_per_profile)
    {
        p->pending_retry_kind = WIFI_POLICY_RETRY_SAME_PROFILE;
        uint32_t delay = wifi_policy_backoff_delay_ms(p->config.retry_base_delay_ms, p->config.retry_max_delay_ms,
                                                        p->current_attempt);
        n = emit(out, max_out, n, act_retry_timer(delay));
        return n;
    }

    return on_profile_exhausted(p, out, max_out, n);
}

uint8_t wifi_policy_handle(wifi_policy_t *p, const wifi_policy_input_t *in, wifi_policy_action_t *out,
                            uint8_t max_out)
{
    uint8_t n = 0;

    switch (in->kind)
    {
    case WIFI_POLICY_IN_STARTED:
        if (p->profile_count == 0)
        {
            p->state = WIFI_POLICY_STATE_READY;
            return emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_READY_NO_PROFILES, NULL));
        }
        return begin_from(p, 0, p->profile_count, out, max_out, n);

    case WIFI_POLICY_IN_CONNECT_SUCCESS:
    {
        if (p->current_ssid[0] == '\0')
        {
            // Stale/spurious success with no active connect attempt (e.g. a
            // duplicate low-level event outside CONNECTING state) - nothing
            // was actually being connected to, so there is nothing valid to
            // mark as last-success. Processing this anyway would persist an
            // empty-SSID profile entry.
            return 0;
        }
        char ssid[WIFI_POLICY_SSID_MAX + 1];
        copy_ssid(ssid, p->current_ssid);
        p->state = WIFI_POLICY_STATE_CONNECTED;
        p->current_attempt = 0;
        p->fallback_pending = false;
        p->fallback_ssid[0] = '\0';

        if (p->current_origin == WIFI_POLICY_ORIGIN_AUTOCONNECT && p->profile_count > 0)
        {
            uint8_t idx = p->order[p->cursor % p->profile_count];
            if (strcmp(p->profiles[idx].ssid, ssid) == 0)
            {
                p->consecutive_auth_fails[idx] = 0;
            }
        }

        n = emit(out, max_out, n, act_mark_last_success(ssid));
        n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_CONNECTED, ssid));
        return n;
    }

    case WIFI_POLICY_IN_CONNECT_FAIL:
        if (p->state != WIFI_POLICY_STATE_CONNECTING)
        {
            return 0;
        }
        return handle_terminal_failure(p, in->fail_class, out, max_out, n);

    case WIFI_POLICY_IN_CONNECT_TIMEOUT:
        if (p->state != WIFI_POLICY_STATE_CONNECTING)
        {
            return 0;
        }
        return handle_terminal_failure(p, WIFI_POLICY_FAIL_OTHER, out, max_out, n);

    case WIFI_POLICY_IN_LINK_LOST:
    {
        if (p->state != WIFI_POLICY_STATE_CONNECTED)
        {
            return 0;
        }
        n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_DISCONNECTED, p->current_ssid));

        bool found = false;
        uint8_t found_pos = 0;
        for (uint8_t pos = 0; pos < p->profile_count; pos++)
        {
            uint8_t idx = p->order[pos];
            if (strcmp(p->profiles[idx].ssid, p->current_ssid) == 0)
            {
                found = true;
                found_pos = pos;
                break;
            }
        }
        if (!found)
        {
            p->state = WIFI_POLICY_STATE_READY;
            p->current_origin = WIFI_POLICY_ORIGIN_NONE;
            p->current_ssid[0] = '\0';
            return n;
        }

        p->current_origin = WIFI_POLICY_ORIGIN_AUTOCONNECT;
        p->current_attempt = 0;
        p->cursor = found_pos;
        p->remaining_in_cycle = p->profile_count;
        p->state = WIFI_POLICY_STATE_CONNECTING;

        n = emit(out, max_out, n, act_connect(p->current_ssid, WIFI_POLICY_ORIGIN_AUTOCONNECT));
        n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_CONNECTING, p->current_ssid));
        return n;
    }

    case WIFI_POLICY_IN_RETRY_TIMER_EXPIRED:
    {
        wifi_policy_retry_kind_t kind = p->pending_retry_kind;
        p->pending_retry_kind = WIFI_POLICY_RETRY_NONE;

        if (kind == WIFI_POLICY_RETRY_SAME_PROFILE)
        {
            p->state = WIFI_POLICY_STATE_CONNECTING;
            n = emit(out, max_out, n, act_connect(p->current_ssid, p->current_origin));
            n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_CONNECTING, p->current_ssid));
            return n;
        }
        if (kind == WIFI_POLICY_RETRY_NEW_CYCLE)
        {
            if (p->profile_count == 0)
            {
                return 0;
            }
            return begin_from(p, 0, p->profile_count, out, max_out, n);
        }
        return 0;
    }

    case WIFI_POLICY_IN_CMD_CONNECT_NEW:
        if (p->state == WIFI_POLICY_STATE_CONNECTED)
        {
            copy_ssid(p->fallback_ssid, p->current_ssid);
            p->fallback_pending = true;
            n = emit(out, max_out, n, act_disconnect());
        }
        else
        {
            p->fallback_pending = false;
        }
        p->current_origin = WIFI_POLICY_ORIGIN_MANUAL;
        copy_ssid(p->current_ssid, in->ssid);
        p->current_attempt = 0;
        p->state = WIFI_POLICY_STATE_CONNECTING;
        n = emit(out, max_out, n, act_connect(p->current_ssid, WIFI_POLICY_ORIGIN_MANUAL));
        n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_CONNECTING, p->current_ssid));
        return n;

    case WIFI_POLICY_IN_CMD_CONNECT_SAVED:
        for (uint8_t i = 0; i < p->profile_count; i++)
        {
            if (strcmp(p->profiles[i].ssid, in->ssid) == 0)
            {
                if (p->profiles[i].blocked)
                {
                    p->profiles[i].blocked = false;
                    n = emit(out, max_out, n, act_set_blocked(p->profiles[i].ssid, false));
                }
                p->consecutive_auth_fails[i] = 0;
                break;
            }
        }
        if (p->state == WIFI_POLICY_STATE_CONNECTED)
        {
            n = emit(out, max_out, n, act_disconnect());
        }
        p->fallback_pending = false;
        p->current_origin = WIFI_POLICY_ORIGIN_MANUAL;
        copy_ssid(p->current_ssid, in->ssid);
        p->current_attempt = 0;
        p->state = WIFI_POLICY_STATE_CONNECTING;
        n = emit(out, max_out, n, act_connect(p->current_ssid, WIFI_POLICY_ORIGIN_MANUAL));
        n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_CONNECTING, p->current_ssid));
        return n;

    case WIFI_POLICY_IN_CMD_DISCONNECT:
        if (p->state == WIFI_POLICY_STATE_CONNECTED || p->state == WIFI_POLICY_STATE_CONNECTING)
        {
            n = emit(out, max_out, n, act_disconnect());
            n = emit(out, max_out, n, act_event(WIFI_POLICY_EVENT_DISCONNECTED, p->current_ssid));
        }
        p->state = WIFI_POLICY_STATE_READY;
        p->current_origin = WIFI_POLICY_ORIGIN_NONE;
        p->current_ssid[0] = '\0';
        p->fallback_pending = false;
        p->pending_retry_kind = WIFI_POLICY_RETRY_NONE;
        return n;

    case WIFI_POLICY_IN_CMD_FORGET:
        if (strcmp(p->current_ssid, in->ssid) == 0 &&
            (p->state == WIFI_POLICY_STATE_CONNECTED || p->state == WIFI_POLICY_STATE_CONNECTING))
        {
            n = emit(out, max_out, n, act_disconnect());
            p->state = WIFI_POLICY_STATE_READY;
            p->current_origin = WIFI_POLICY_ORIGIN_NONE;
            p->current_ssid[0] = '\0';
        }
        return n;

    case WIFI_POLICY_IN_CMD_UPDATE_PASSWORD:
        for (uint8_t i = 0; i < p->profile_count; i++)
        {
            if (strcmp(p->profiles[i].ssid, in->ssid) == 0)
            {
                if (p->profiles[i].blocked)
                {
                    p->profiles[i].blocked = false;
                    n = emit(out, max_out, n, act_set_blocked(p->profiles[i].ssid, false));
                }
                p->consecutive_auth_fails[i] = 0;
                break;
            }
        }
        return n;

    default:
        return 0;
    }
}

wifi_policy_state_t wifi_policy_state(const wifi_policy_t *p)
{
    return p->state;
}

uint32_t wifi_policy_backoff_delay_ms(uint32_t base_ms, uint32_t max_ms, uint8_t attempt)
{
    if (max_ms < base_ms)
    {
        max_ms = base_ms;
    }
    uint64_t delay = base_ms;
    for (uint8_t i = 0; i < attempt && delay < max_ms; i++)
    {
        delay *= 2;
    }
    if (delay > max_ms)
    {
        delay = max_ms;
    }
    return (uint32_t)delay;
}

wifi_policy_fail_class_t wifi_policy_classify_reason(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_POLICY_REASON_AUTH_EXPIRE:
    case WIFI_POLICY_REASON_MIC_FAILURE:
    case WIFI_POLICY_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_POLICY_REASON_AUTH_FAIL:
    case WIFI_POLICY_REASON_HANDSHAKE_TIMEOUT:
        return WIFI_POLICY_FAIL_AUTH;
    case WIFI_POLICY_REASON_NO_AP_FOUND:
        return WIFI_POLICY_FAIL_AP_NOT_FOUND;
    default:
        return WIFI_POLICY_FAIL_OTHER;
    }
}

uint8_t wifi_policy_sort_scan(wifi_policy_scan_ap_t *aps, uint8_t count, uint8_t max_out,
                               const wifi_policy_profile_t *profiles, uint8_t profile_count,
                               const char *connected_ssid)
{
    uint8_t unique_count = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        bool merged = false;
        for (uint8_t j = 0; j < unique_count; j++)
        {
            if (strcmp(aps[j].ssid, aps[i].ssid) == 0)
            {
                // Only rssi is refreshed from the stronger duplicate BSSID -
                // secured (and any other non-rssi field) keeps whatever the
                // first-seen BSSID had. Edge case (same SSID broadcast with
                // different security by multiple BSSIDs), not worth the
                // extra bookkeeping here.
                if (aps[i].rssi > aps[j].rssi)
                {
                    aps[j].rssi = aps[i].rssi;
                }
                merged = true;
                break;
            }
        }
        if (!merged)
        {
            if (unique_count != i)
            {
                aps[unique_count] = aps[i];
            }
            unique_count++;
        }
    }

    for (uint8_t i = 1; i < unique_count; i++)
    {
        wifi_policy_scan_ap_t key = aps[i];
        int16_t j = (int16_t)i - 1;
        while (j >= 0 && aps[j].rssi < key.rssi)
        {
            aps[j + 1] = aps[j];
            j--;
        }
        aps[j + 1] = key;
    }

    if (unique_count > max_out)
    {
        unique_count = max_out;
    }

    for (uint8_t i = 0; i < unique_count; i++)
    {
        aps[i].saved = false;
        for (uint8_t j = 0; j < profile_count; j++)
        {
            if (strcmp(aps[i].ssid, profiles[j].ssid) == 0)
            {
                aps[i].saved = true;
                break;
            }
        }
        aps[i].connected = (connected_ssid != NULL) && (strcmp(aps[i].ssid, connected_ssid) == 0);
    }

    return unique_count;
}
