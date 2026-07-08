#include "test_common.h"
#include "wifi_policy.h"

static wifi_policy_config_t default_config(void)
{
    wifi_policy_config_t cfg = {
        .retry_base_delay_ms = 1000,
        .retry_max_delay_ms = 8000,
        .max_attempts_per_profile = 3,
        .auth_block_threshold = 5, // high: not exercised by this suite
        .inter_cycle_delay_ms = 100,
    };
    return cfg;
}

static void test_pause_while_connecting_aborts_without_event(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = false}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);

    wifi_policy_input_t pause = {.kind = WIFI_POLICY_IN_CMD_PAUSE_AUTOCONNECT};
    uint8_t n = wifi_policy_handle(&p, &pause, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(n == 1);
    CHECK(actions[0].kind == WIFI_POLICY_ACT_DISCONNECT);
    for (uint8_t i = 0; i < n; i++)
    {
        CHECK(actions[i].kind != WIFI_POLICY_ACT_EMIT_EVENT);
    }
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
    CHECK_STREQ(p.current_ssid, "");
}

static void test_pause_while_connected_is_noop(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = false}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t success = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
    wifi_policy_handle(&p, &success, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);

    wifi_policy_input_t pause = {.kind = WIFI_POLICY_IN_CMD_PAUSE_AUTOCONNECT};
    uint8_t n = wifi_policy_handle(&p, &pause, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(n == 0);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);
    CHECK_STREQ(p.current_ssid, "AAA");
}

static void test_pause_while_ready_cancels_pending_retry(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    cfg.max_attempts_per_profile = 1; // exhaust the single profile immediately
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = false}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);

    // Single profile, single attempt budget: failing exhausts the cycle and
    // schedules the inter-cycle retry timer, landing in READY with a pending
    // restart - exactly the "looping against unreachable saved networks"
    // state this feature targets.
    wifi_policy_input_t fail = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_OTHER};
    wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
    CHECK(p.pending_retry_kind == WIFI_POLICY_RETRY_NEW_CYCLE);

    wifi_policy_input_t pause = {.kind = WIFI_POLICY_IN_CMD_PAUSE_AUTOCONNECT};
    uint8_t n = wifi_policy_handle(&p, &pause, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(n == 0); // nothing in flight at the radio level to abort
    CHECK(p.pending_retry_kind == WIFI_POLICY_RETRY_NONE);

    // The adapter's retry timer may already be armed and fire after pause;
    // policy must treat that stale expiry as a no-op.
    wifi_policy_input_t retry_timer = {.kind = WIFI_POLICY_IN_RETRY_TIMER_EXPIRED};
    n = wifi_policy_handle(&p, &retry_timer, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(n == 0);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
}

static void test_resume_from_ready_restarts_cycle(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = false}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t pause = {.kind = WIFI_POLICY_IN_CMD_PAUSE_AUTOCONNECT};
    wifi_policy_handle(&p, &pause, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);

    wifi_policy_input_t resume = {.kind = WIFI_POLICY_IN_CMD_RESUME_AUTOCONNECT};
    uint8_t n = wifi_policy_handle(&p, &resume, actions, WIFI_POLICY_MAX_ACTIONS);

    bool connecting_to_aaa = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_CONNECT && strcmp(actions[i].ssid, "AAA") == 0)
        {
            connecting_to_aaa = true;
        }
    }
    CHECK(connecting_to_aaa);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);
}

static void test_resume_while_connected_or_connecting_is_noop(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = false}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);

    wifi_policy_input_t resume = {.kind = WIFI_POLICY_IN_CMD_RESUME_AUTOCONNECT};
    uint8_t n = wifi_policy_handle(&p, &resume, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(n == 0);
    CHECK_STREQ(p.current_ssid, "AAA"); // untouched, still the original in-flight attempt

    wifi_policy_input_t success = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
    wifi_policy_handle(&p, &success, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);

    n = wifi_policy_handle(&p, &resume, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(n == 0);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);
}

static void test_resume_with_no_profiles_is_noop(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY); // READY_NO_PROFILES

    wifi_policy_input_t resume = {.kind = WIFI_POLICY_IN_CMD_RESUME_AUTOCONNECT};
    uint8_t n = wifi_policy_handle(&p, &resume, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(n == 0);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
}

int main(void)
{
    test_pause_while_connecting_aborts_without_event();
    test_pause_while_connected_is_noop();
    test_pause_while_ready_cancels_pending_retry();
    test_resume_from_ready_restarts_cycle();
    test_resume_while_connected_or_connecting_is_noop();
    test_resume_with_no_profiles_is_noop();
    return test_summary("wifi_policy_pause_resume");
}
