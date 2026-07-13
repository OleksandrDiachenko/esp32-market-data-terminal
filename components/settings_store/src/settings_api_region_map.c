#include "settings_api_region_map.h"

#include <string.h>

// Exact "Zone/City" labels main/display_ui.c's tz table produces for every
// U.S. state and populated U.S. territory it currently offers a city entry
// for. Deliberately an explicit list, not a zone or "America/*" prefix rule -
// see settings_api_region_map.h and
// docs/decisions/0009-regional-server-auto-selection.md for why.
static const char *const s_us_tz_labels[] = {
    "America/Anchorage",   // Alaska
    "America/Los Angeles", // Pacific
    "America/Denver",      // Mountain
    "America/Phoenix",     // Mountain, no DST (Arizona)
    "America/Chicago",     // Central
    "America/Dallas",      // Central
    "America/New York",    // Eastern
    "Pacific/Honolulu",    // Hawaii
    "America/San Juan",    // Puerto Rico
    "America/St Thomas",   // U.S. Virgin Islands
    "Pacific/Guam",        // Guam
    "Pacific/Saipan",      // Northern Mariana Islands
    "Pacific/Pago Pago",   // American Samoa
};

settings_api_region_t settings_api_region_from_tz_label(const char *tz_label)
{
    if (tz_label == NULL)
    {
        return SETTINGS_API_REGION_INTL;
    }

    for (size_t i = 0; i < sizeof(s_us_tz_labels) / sizeof(s_us_tz_labels[0]); i++)
    {
        if (strcmp(tz_label, s_us_tz_labels[i]) == 0)
        {
            return SETTINGS_API_REGION_US;
        }
    }
    return SETTINGS_API_REGION_INTL;
}
