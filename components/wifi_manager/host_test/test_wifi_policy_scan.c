#include "test_common.h"
#include "wifi_policy.h"

static void test_sort_dedupe_cap(void)
{
    wifi_policy_scan_ap_t aps[5] = {
        {.ssid = "A", .rssi = -70},
        {.ssid = "B", .rssi = -40},
        {.ssid = "A", .rssi = -50}, // duplicate SSID, stronger signal
        {.ssid = "C", .rssi = -60},
        {.ssid = "D", .rssi = -90},
    };

    wifi_policy_profile_t profiles[1] = {{.ssid = "B", .blocked = false}};

    uint8_t count = wifi_policy_sort_scan(aps, 5, 3, profiles, 1, "C");

    CHECK(count == 3);
    CHECK_STREQ(aps[0].ssid, "B");
    CHECK(aps[0].rssi == -40);
    CHECK(aps[0].saved);
    CHECK(!aps[0].connected);

    CHECK_STREQ(aps[1].ssid, "A");
    CHECK(aps[1].rssi == -50);
    CHECK(!aps[1].saved);

    CHECK_STREQ(aps[2].ssid, "C");
    CHECK(aps[2].connected);
}

static void test_empty_scan(void)
{
    wifi_policy_scan_ap_t aps[1] = {{.ssid = "", .rssi = 0, .saved = false, .connected = false}};
    uint8_t count = wifi_policy_sort_scan(aps, 0, 20, NULL, 0, NULL);
    CHECK(count == 0);
}

// secured is copied around by whole-struct assignment (see sort/cap in
// wifi_policy_sort_scan()), so it should survive re-sorting untouched. The
// dedupe merge only refreshes rssi from a stronger duplicate BSSID, not
// secured - documented as a deliberate edge case in wifi_policy.c.
static void test_secured_field_preserved(void)
{
    wifi_policy_scan_ap_t aps[3] = {
        {.ssid = "Open", .rssi = -50, .secured = false},
        {.ssid = "Secure", .rssi = -60, .secured = true},
        {.ssid = "Secure", .rssi = -40, .secured = false}, // duplicate SSID, stronger signal, different secured value
    };

    uint8_t count = wifi_policy_sort_scan(aps, 3, 10, NULL, 0, NULL);

    CHECK(count == 2);
    CHECK_STREQ(aps[0].ssid, "Secure");
    CHECK(aps[0].rssi == -40);  // rssi refreshed from the stronger duplicate
    CHECK(aps[0].secured);      // secured NOT refreshed - keeps the first-seen BSSID's value

    CHECK_STREQ(aps[1].ssid, "Open");
    CHECK(!aps[1].secured);
}

int main(void)
{
    test_sort_dedupe_cap();
    test_empty_scan();
    test_secured_field_preserved();
    return test_summary("wifi_policy_scan");
}
