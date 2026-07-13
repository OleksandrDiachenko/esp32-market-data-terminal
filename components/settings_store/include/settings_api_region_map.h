#pragma once

// Pure C, host-compilable: maps a locale_settings_t.tz_label ("Zone/City",
// e.g. "America/Chicago") to the Binance API region it implies. An
// explicit allow-list, not a zone-id or "America/*" prefix rule - the
// existing tz table files Canada/Mexico/Central/South America under the
// same zone as U.S. cities, and Hawaii under a different zone entirely -
// see docs/decisions/0009-regional-server-auto-selection.md.

#include "settings_codec.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Returns SETTINGS_API_REGION_US if tz_label is one of the U.S.
    // state/territory labels in the allow-list, SETTINGS_API_REGION_INTL
    // otherwise (including NULL or an empty/never-set label).
    settings_api_region_t settings_api_region_from_tz_label(const char *tz_label);

#ifdef __cplusplus
}
#endif
