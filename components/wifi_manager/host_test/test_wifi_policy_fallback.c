#include "test_common.h"
#include "wifi_policy.h"

static wifi_policy_config_t default_config(void)
{
    wifi_policy_config_t cfg = {
        .retry_base_delay_ms = 100,
        .retry_max_delay_ms = 800,
        .max_attempts_per_profile = 3,
        .auth_block_threshold = 2,
        .inter_cycle_delay_ms = 50,
    };
    return cfg;
}

static void connect_new_and_succeed(wifi_policy_t *p, const char *ssid)
{
    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t connect = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect.ssid, ssid, WIFI_POLICY_SSID_MAX);
    wifi_policy_handle(p, &connect, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t success = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
    wifi_policy_handle(p, &success, actions, WIFI_POLICY_MAX_ACTIONS);
}

static void test_fallback_on_auth_fail(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    connect_new_and_succeed(&p, "A");
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t connect_b = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect_b.ssid, "B", WIFI_POLICY_SSID_MAX);
    uint8_t n = wifi_policy_handle(&p, &connect_b, actions, WIFI_POLICY_MAX_ACTIONS);

    bool saw_disconnect = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_DISCONNECT)
        {
            saw_disconnect = true;
        }
    }
    CHECK(saw_disconnect);
    CHECK_STREQ(p.current_ssid, "B");
    CHECK_STREQ(p.fallback_ssid, "A");
    CHECK(p.fallback_pending);

    wifi_policy_input_t fail_b = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_AUTH};
    n = wifi_policy_handle(&p, &fail_b, actions, WIFI_POLICY_MAX_ACTIONS);

    bool saw_fallback_started = false;
    bool saw_connect_a = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_EMIT_EVENT && actions[i].event == WIFI_POLICY_EVENT_FALLBACK_STARTED)
        {
            saw_fallback_started = true;
        }
        if (actions[i].kind == WIFI_POLICY_ACT_CONNECT && strcmp(actions[i].ssid, "A") == 0)
        {
            saw_connect_a = true;
        }
    }
    CHECK(saw_fallback_started);
    CHECK(saw_connect_a);
    CHECK_STREQ(p.current_ssid, "A");
    CHECK(!p.fallback_pending);
}

static void test_fallback_on_timeout(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    connect_new_and_succeed(&p, "A");

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t connect_b = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect_b.ssid, "B", WIFI_POLICY_SSID_MAX);
    wifi_policy_handle(&p, &connect_b, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t timeout_b = {.kind = WIFI_POLICY_IN_CONNECT_TIMEOUT};
    uint8_t n = wifi_policy_handle(&p, &timeout_b, actions, WIFI_POLICY_MAX_ACTIONS);

    bool saw_connect_a = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_CONNECT && strcmp(actions[i].ssid, "A") == 0)
        {
            saw_connect_a = true;
        }
    }
    CHECK(saw_connect_a);
    CHECK_STREQ(p.current_ssid, "A");
}

static void test_new_network_success_no_fallback(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    connect_new_and_succeed(&p, "A");

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t connect_b = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect_b.ssid, "B", WIFI_POLICY_SSID_MAX);
    wifi_policy_handle(&p, &connect_b, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t success = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
    uint8_t n = wifi_policy_handle(&p, &success, actions, WIFI_POLICY_MAX_ACTIONS);

    bool saw_mark_last_success_b = false;
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == WIFI_POLICY_ACT_MARK_LAST_SUCCESS && strcmp(actions[i].ssid, "B") == 0)
        {
            saw_mark_last_success_b = true;
        }
    }
    CHECK(saw_mark_last_success_b);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);
    CHECK(!p.fallback_pending);
}

static void test_fallback_attempt_fails_then_settles(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    connect_new_and_succeed(&p, "A");

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t connect_b = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect_b.ssid, "B", WIFI_POLICY_SSID_MAX);
    wifi_policy_handle(&p, &connect_b, actions, WIFI_POLICY_MAX_ACTIONS);

    wifi_policy_input_t fail_b = {.kind = WIFI_POLICY_IN_CONNECT_FAIL, .fail_class = WIFI_POLICY_FAIL_OTHER};
    wifi_policy_handle(&p, &fail_b, actions, WIFI_POLICY_MAX_ACTIONS); // fallback to A starts
    CHECK_STREQ(p.current_ssid, "A");
    CHECK(p.current_origin == WIFI_POLICY_ORIGIN_FALLBACK);

    // The fallback attempt to A also fails. There is no further fallback
    // and A isn't a known profile, so policy must settle into READY
    // instead of looping or crashing.
    wifi_policy_handle(&p, &fail_b, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);
}

int main(void)
{
    test_fallback_on_auth_fail();
    test_fallback_on_timeout();
    test_new_network_success_no_fallback();
    test_fallback_attempt_fails_then_settles();
    return test_summary("wifi_policy_fallback");
}
