#include "test_common.h"
#include "wifi_policy.h"

static wifi_policy_config_t default_config(void)
{
    wifi_policy_config_t cfg = {
        .retry_base_delay_ms = 1000,
        .retry_max_delay_ms = 8000,
        .max_attempts_per_profile = 3,
        .auth_block_threshold = 2,
        .inter_cycle_delay_ms = 100,
    };
    return cfg;
}

static void test_boot_no_profiles(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_STARTED};
    uint8_t n = wifi_policy_handle(&p, &in, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(n == 1);
    CHECK(actions[0].kind == WIFI_POLICY_ACT_EMIT_EVENT);
    CHECK(actions[0].event == WIFI_POLICY_EVENT_READY_NO_PROFILES);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
}

static void test_boot_autoconnect_order(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[3] = {
        {.ssid = "AAA", .blocked = false},
        {.ssid = "BBB", .blocked = false},
        {.ssid = "CCC", .blocked = false},
    };
    wifi_policy_set_profiles(&p, profiles, 3, "BBB");

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_STARTED};
    uint8_t n = wifi_policy_handle(&p, &in, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(n >= 1);
    CHECK(actions[0].kind == WIFI_POLICY_ACT_CONNECT);
    CHECK_STREQ(actions[0].ssid, "BBB");
    CHECK(actions[0].origin == WIFI_POLICY_ORIGIN_AUTOCONNECT);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);
}

static void test_boot_skips_blocked_and_no_last_success(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[2] = {
        {.ssid = "AAA", .blocked = true},
        {.ssid = "BBB", .blocked = false},
    };
    wifi_policy_set_profiles(&p, profiles, 2, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t in = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &in, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(actions[0].kind == WIFI_POLICY_ACT_CONNECT);
    CHECK_STREQ(actions[0].ssid, "BBB");
}

int main(void)
{
    test_boot_no_profiles();
    test_boot_autoconnect_order();
    test_boot_skips_blocked_and_no_last_success();
    return test_summary("wifi_policy_boot");
}
