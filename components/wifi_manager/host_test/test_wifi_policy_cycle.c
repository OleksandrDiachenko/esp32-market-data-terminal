#include "test_common.h"
#include "wifi_policy.h"

static wifi_policy_config_t fast_config(void)
{
    wifi_policy_config_t cfg = {
        .retry_base_delay_ms = 10,
        .retry_max_delay_ms = 20,
        .max_attempts_per_profile = 1, // exhaust immediately, for a tight test loop
        .auth_block_threshold = 100,   // never block via this path
        .inter_cycle_delay_ms = 5,
    };
    return cfg;
}

static void fail_current(wifi_policy_t *p)
{
    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t fail = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_OTHER};
    wifi_policy_handle(p, &fail, actions, WIFI_POLICY_MAX_ACTIONS);
}

static void expire_retry_timer(wifi_policy_t *p)
{
    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t t = {.kind = WIFI_POLICY_IN_RETRY_TIMER_EXPIRED};
    wifi_policy_handle(p, &t, actions, WIFI_POLICY_MAX_ACTIONS);
}

static void test_cycle_restarts_and_repeats(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = fast_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[2] = {
        {.ssid = "AAA", .blocked = false},
        {.ssid = "BBB", .blocked = false},
    };
    wifi_policy_set_profiles(&p, profiles, 2, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);

    // 50 full cycles: each profile fails once per cycle (max_attempts=1),
    // the cycle must keep restarting cleanly with no state corruption.
    for (int cycle = 0; cycle < 50; cycle++)
    {
        for (int i = 0; i < 2; i++)
        {
            bool valid_ssid = (strcmp(p.current_ssid, "AAA") == 0) || (strcmp(p.current_ssid, "BBB") == 0);
            CHECK(valid_ssid);
            fail_current(&p);
        }

        CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
        expire_retry_timer(&p);
        CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);
    }
}

static void test_all_blocked_goes_ready_without_looping(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = fast_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[2] = {
        {.ssid = "AAA", .blocked = true},
        {.ssid = "BBB", .blocked = true},
    };
    wifi_policy_set_profiles(&p, profiles, 2, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    uint8_t n = wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
    bool saw_all_blocked = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_EMIT_EVENT &&
            actions[i].event == WIFI_POLICY_EVENT_ALL_PROFILES_BLOCKED)
        {
            saw_all_blocked = true;
        }
        CHECK(actions[i].kind != WIFI_POLICY_ACT_CONNECT);
    }
    CHECK(saw_all_blocked);
}

int main(void)
{
    test_cycle_restarts_and_repeats();
    test_all_blocked_goes_ready_without_looping();
    return test_summary("wifi_policy_cycle");
}
