#include "test_common.h"
#include "settings_api_region_map.h"

static void test_us_states_and_territories_map_to_us(void)
{
    static const char *const us_labels[] = {
        "America/Anchorage", "America/Los Angeles", "America/Denver",    "America/Phoenix",  "America/Chicago",
        "America/Dallas",    "America/New York",    "Pacific/Honolulu",  "America/San Juan", "America/St Thomas",
        "Pacific/Guam",      "Pacific/Saipan",      "Pacific/Pago Pago",
    };
    for (size_t i = 0; i < sizeof(us_labels) / sizeof(us_labels[0]); i++)
    {
        CHECK(settings_api_region_from_tz_label(us_labels[i]) == SETTINGS_API_REGION_US);
    }
}

// Same TZ_ZONE_AMERICA bucket as the U.S. cities above in main/display_ui.c's
// tz table - proves the mapping is an explicit allow-list, not a
// zone-id/prefix rule that would also match these.
static void test_non_us_americas_map_to_intl(void)
{
    static const char *const non_us_labels[] = {
        "America/Toronto",  "America/Vancouver",    "America/Calgary",   "America/Winnipeg",  "America/Halifax",
        "America/St Johns", "America/Mexico City",  "America/Havana",    "America/Guatemala", "America/Panama",
        "America/Bogota",   "America/Buenos Aires", "America/Sao Paulo",
    };
    for (size_t i = 0; i < sizeof(non_us_labels) / sizeof(non_us_labels[0]); i++)
    {
        CHECK(settings_api_region_from_tz_label(non_us_labels[i]) == SETTINGS_API_REGION_INTL);
    }
}

static void test_other_zones_map_to_intl(void)
{
    CHECK(settings_api_region_from_tz_label("Europe/Kyiv") == SETTINGS_API_REGION_INTL);
    CHECK(settings_api_region_from_tz_label("Asia/Tokyo") == SETTINGS_API_REGION_INTL);
}

static void test_empty_and_null_label_map_to_intl(void)
{
    CHECK(settings_api_region_from_tz_label("") == SETTINGS_API_REGION_INTL);
    CHECK(settings_api_region_from_tz_label(NULL) == SETTINGS_API_REGION_INTL);
}

int main(void)
{
    test_us_states_and_territories_map_to_us();
    test_non_us_americas_map_to_intl();
    test_other_zones_map_to_intl();
    test_empty_and_null_label_map_to_intl();
    return test_summary("settings_api_region_map");
}
