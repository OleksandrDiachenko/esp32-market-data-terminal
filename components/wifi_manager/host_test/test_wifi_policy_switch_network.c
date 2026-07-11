#include "test_common.h"
#include "wifi_policy.h"

// Covers the race that used to make "Add Network" fail almost instantly:
// switching networks while already CONNECTED tears down the old association
// asynchronously, and the policy must not mistake that stray disconnect for
// the new attempt failing (see WIFI_POLICY_IN_TEARDOWN_DISCONNECTED).

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

static bool has_action(const wifi_policy_action_t *actions, uint8_t n, wifi_policy_action_kind_t kind,
                        const char *ssid)
{
    for (uint8_t i = 0; i < n; i++)
    {
        if (actions[i].kind == kind && (ssid == NULL || strcmp(actions[i].ssid, ssid) == 0))
        {
            return true;
        }
    }
    return false;
}

// Switching to a new network while already CONNECTED must defer the actual
// connect until the old association's teardown is confirmed.
static void test_connect_new_while_connected_defers_connect(void)
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

    CHECK(has_action(actions, n, WIFI_POLICY_ACT_DISCONNECT, NULL));
    CHECK(!has_action(actions, n, WIFI_POLICY_ACT_CONNECT, NULL));
    CHECK(wifi_policy_is_teardown_pending(&p));
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);
    CHECK_STREQ(p.current_ssid, "B");
}

// The old AP's stray disconnect landing after state is already CONNECTING
// must start the deferred connect to the new SSID, not fail the attempt.
static void test_teardown_disconnected_starts_deferred_connect(void)
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

    wifi_policy_input_t teardown_done = {.kind = WIFI_POLICY_IN_TEARDOWN_DISCONNECTED};
    uint8_t n = wifi_policy_handle(&p, &teardown_done, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(has_action(actions, n, WIFI_POLICY_ACT_CONNECT, "B"));
    CHECK(!wifi_policy_is_teardown_pending(&p));
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);

    wifi_policy_input_t success = {.kind = WIFI_POLICY_IN_CONNECT_SUCCESS};
    n = wifi_policy_handle(&p, &success, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);
    CHECK_STREQ(p.current_ssid, "B");
}

// A stray/duplicate TEARDOWN_DISCONNECTED with nothing pending is a no-op.
static void test_teardown_disconnected_noop_when_not_pending(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    connect_new_and_succeed(&p, "A");

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t teardown_done = {.kind = WIFI_POLICY_IN_TEARDOWN_DISCONNECTED};
    uint8_t n = wifi_policy_handle(&p, &teardown_done, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(n == 0);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTED);
    CHECK_STREQ(p.current_ssid, "A");
}

// If the old association's disconnect event never arrives, the connect
// timeout must still resolve the attempt instead of hanging forever, and
// must clear teardown_pending.
static void test_connect_timeout_while_teardown_pending_resolves(void)
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
    CHECK(wifi_policy_is_teardown_pending(&p));

    wifi_policy_input_t timeout_b = {.kind = WIFI_POLICY_IN_CONNECT_TIMEOUT};
    uint8_t n = wifi_policy_handle(&p, &timeout_b, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(!wifi_policy_is_teardown_pending(&p));
    // B has no fallback profile of its own, but A is still the fallback
    // target recorded when the switch to B began.
    CHECK(has_action(actions, n, WIFI_POLICY_ACT_CONNECT, "A"));
    CHECK_STREQ(p.current_ssid, "A");
}

// Cold-connect path (nothing currently connected) is unaffected: connect is
// still issued immediately, with no teardown wait.
static void test_connect_new_while_idle_connects_immediately(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    wifi_policy_input_t started = {.kind = WIFI_POLICY_IN_STARTED};
    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_handle(&p, &started, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_READY);

    wifi_policy_input_t connect_a = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect_a.ssid, "A", WIFI_POLICY_SSID_MAX);
    uint8_t n = wifi_policy_handle(&p, &connect_a, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(has_action(actions, n, WIFI_POLICY_ACT_CONNECT, "A"));
    CHECK(!has_action(actions, n, WIFI_POLICY_ACT_DISCONNECT, NULL));
    CHECK(!wifi_policy_is_teardown_pending(&p));
}

// Switching targets again while a manual connect is still in flight (state
// CONNECTING, not yet CONNECTED - e.g. the user taps a second network before
// the first attempt resolves) must also defer via teardown, not stack a new
// esp_wifi_connect() on top of the still-live one. The adapter no longer
// disconnects on our behalf before every connect (see wifi_manager.c
// execute_connect), so the policy must ask for it explicitly here too.
static void test_connect_new_while_connecting_defers_connect(void)
{
    wifi_policy_t p;
    wifi_policy_config_t cfg = default_config();
    wifi_policy_init(&p, &cfg);
    wifi_policy_set_profiles(&p, NULL, 0, NULL);

    wifi_policy_action_t actions[WIFI_POLICY_MAX_ACTIONS];
    wifi_policy_input_t connect_a = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect_a.ssid, "A", WIFI_POLICY_SSID_MAX);
    wifi_policy_handle(&p, &connect_a, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);
    CHECK(!wifi_policy_is_teardown_pending(&p));

    wifi_policy_input_t connect_b = {.kind = WIFI_POLICY_IN_CMD_CONNECT_NEW};
    strncpy(connect_b.ssid, "B", WIFI_POLICY_SSID_MAX);
    uint8_t n = wifi_policy_handle(&p, &connect_b, actions, WIFI_POLICY_MAX_ACTIONS);

    CHECK(has_action(actions, n, WIFI_POLICY_ACT_DISCONNECT, NULL));
    CHECK(!has_action(actions, n, WIFI_POLICY_ACT_CONNECT, NULL));
    CHECK(wifi_policy_is_teardown_pending(&p));
    CHECK(wifi_policy_state(&p) == WIFI_POLICY_STATE_CONNECTING);
    CHECK_STREQ(p.current_ssid, "B");

    wifi_policy_input_t teardown_done = {.kind = WIFI_POLICY_IN_TEARDOWN_DISCONNECTED};
    n = wifi_policy_handle(&p, &teardown_done, actions, WIFI_POLICY_MAX_ACTIONS);
    CHECK(has_action(actions, n, WIFI_POLICY_ACT_CONNECT, "B"));
    CHECK(!wifi_policy_is_teardown_pending(&p));
}

int main(void)
{
    test_connect_new_while_connected_defers_connect();
    test_teardown_disconnected_starts_deferred_connect();
    test_teardown_disconnected_noop_when_not_pending();
    test_connect_timeout_while_teardown_pending_resolves();
    test_connect_new_while_idle_connects_immediately();
    test_connect_new_while_connecting_defers_connect();
    return test_summary("wifi_policy_switch_network");
}
