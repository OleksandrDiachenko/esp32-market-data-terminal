#include "test_common.h"
#include "wifi_policy.h"

static wifi_policy_config_t default_config(void)
{
    wifi_policy_config_t cfg = {
        .retry_base_delay_ms = 100,
        .retry_max_delay_ms = 800,
        .max_attempts_per_profile = 5, // high: attempt limit shouldn't interfere here
        .auth_block_threshold = 2,
        .inter_cycle_delay_ms = 50,
    };
    return cfg;
}

static void test_auth_fail_below_threshold_does_not_block(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = false}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t start = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_handle(&p, &start, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t fail = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_AUTH};
    uint8_t n = wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(!p.profiles[0].blocked);
    for (uint8_t i = 0; i < n; i++)
    {
        CHECK(actions[i].kind != WIFI_POLICY_ACT_SET_BLOCKED);
    }
}

static void test_auth_fail_at_threshold_blocks(void)
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

    wifi_policy_input_t fail = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_AUTH};
    wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS); // 1st auth fail

    uint8_t n = wifi_policy_handle(&p, &fail, actions, WIFI_POLICY_MAX_ACTIONS); // 2nd -> blocks

    bool saw_set_blocked = false;
    bool saw_auth_event = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_SET_BLOCKED && actions[i].flag && strcmp(actions[i].ssid, "AAA") == 0)
        {
            saw_set_blocked = true;
        }
        if (actions[i].kind == WIFI_POLICY_ACT_EMIT_EVENT && actions[i].event == WIFI_POLICY_EVENT_AUTH_FAILED)
        {
            saw_auth_event = true;
        }
    }
    CHECK(saw_set_blocked);
    CHECK(saw_auth_event);
    CHECK(p.profiles[0].blocked);
    CHECK_STREQ(p.current_ssid, "BBB");
}

static void test_update_password_unblocks(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = true}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t update = {.kind = WIFI_POLICY_IN_CMD_UPDATE_PASSWORD, .ssid = "AAA"};
    uint8_t n = wifi_policy_handle(&p, &update, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(!p.profiles[0].blocked);
    bool saw_unblock = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_SET_BLOCKED && !actions[i].flag)
        {
            saw_unblock = true;
        }
    }
    CHECK(saw_unblock);
}

static void test_connect_saved_on_blocked_profile_unblocks_and_attempts(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);

    wifi_policy_profile_t profiles[1] = {{.ssid = "AAA", .blocked = true}};
    wifi_policy_set_profiles(&p, profiles, 1, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t connect_saved = {.kind = WIFI_POLICY_IN_CMD_CONNECT_SAVED, .ssid = "AAA"};
    uint8_t n = wifi_policy_handle(&p, &connect_saved, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(!p.profiles[0].blocked);
    bool saw_connect = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_CONNECT && strcmp(actions[i].ssid, "AAA") == 0)
        {
            saw_connect = true;
        }
    }
    CHECK(saw_connect);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);

    wifi_policy_input_t success = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
    wifi_policy_handle(&p, &success, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);
}

int main(void)
{
    test_auth_fail_below_threshold_does_not_block();
    test_auth_fail_at_threshold_blocks();
    test_update_password_unblocks();
    test_connect_saved_on_blocked_profile_unblocks_and_attempts();
    return test_summary("wifi_policy_authblock");
}
