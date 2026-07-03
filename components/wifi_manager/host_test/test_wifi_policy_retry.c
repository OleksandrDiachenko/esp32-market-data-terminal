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

static void test_backoff_formula(void)
{
    CHECK(wifi_policy_backoff_delay_ms(1000, 8000, 0) == 1000);
    CHECK(wifi_policy_backoff_delay_ms(1000, 8000, 1) == 2000);
    CHECK(wifi_policy_backoff_delay_ms(1000, 8000, 2) == 4000);
    CHECK(wifi_policy_backoff_delay_ms(1000, 8000, 3) == 8000);
    CHECK(wifi_policy_backoff_delay_ms(1000, 8000, 10) == 8000);
}

static void test_attempt_limit_advances_profile(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[2] = {
        {.ssid = "AAA", .blocked = false},
        {.ssid = "BBB", .blocked = false},
    };
    wifi_policy_set_profiles(&p, profiles, 2, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK_STREQ(p.current_ssid, "AAA");

    wifi_policy_input_t fail = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_OTHER};
    wifi_policy_input_t retry_timer = {.kind = WIFI_POLICY_IN_RETRY_TIMER_EXPIRED};

    uint8_t n = wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS); // attempt 1/3
    CHECK(actions[n - 1].kind == WIFI_POLICY_ACT_START_RETRY_TIMER);
    CHECK_STREQ(p.current_ssid, "AAA");
    wifi_policy_handle(&p, &retry_timer, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK_STREQ(p.current_ssid, "AAA");

    n = wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS); // attempt 2/3
    CHECK(actions[n - 1].kind == WIFI_POLICY_ACT_START_RETRY_TIMER);
    wifi_policy_handle(&p, &retry_timer, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK_STREQ(p.current_ssid, "AAA");

    n = wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS); // attempt 3/3, exhausted
    bool moved_to_bbb = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_CONNECT && strcmp(actions[i].ssid, "BBB") == 0)
        {
            moved_to_bbb = true;
        }
    }
    CHECK(moved_to_bbb);
    CHECK_STREQ(p.current_ssid, "BBB");
}

static void test_success_resets_attempt_counter(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = false}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t fail = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_OTHER};
    wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(p.current_attempt == 1);

    wifi_policy_input_t retry_timer = {.kind = WIFI_POLICY_IN_RETRY_TIMER_EXPIRED};
    wifi_policy_handle(&p, &retry_timer, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t success = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
    wifi_policy_handle(&p, &success, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(p.current_attempt == 0);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);
}

int main(void)
{
    test_backoff_formula();
    test_attempt_limit_advances_profile();
    test_success_resets_attempt_counter();
    return test_summary("wifi_policy_retry");
}
