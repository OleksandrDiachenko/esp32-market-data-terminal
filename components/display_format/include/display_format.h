#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Formats a price-like value for a fixed-width UI field (used for
    // last/hi/lo in both the watchlist row and the add-symbol match card):
    //  - |value| >= 1e9 (more than 9 integer digits): delegates to
    //    display_format_abbreviate(), e.g. "1.25T".
    //  - 1.0 <= |value| < 1e9: fixed 2 decimals, e.g. "43250.12".
    //  - |value| < 1.0: adaptive 4-8 decimals that grow as the value shrinks,
    //    so micro-cap prices (e.g. SHIB, PEPE) keep meaningful digits instead
    //    of rounding to "0.0000", e.g. "0.00000734".
    // out_len should be at least 24 bytes to guarantee no truncation.
    void display_format_price(double value, char *out, size_t out_len);

    // Picks the largest applicable K/M/B/T suffix for `value` and formats it as
    // "<2-decimal mantissa><suffix>", e.g. 1.25e12 -> "1.25T", 5.5e7 -> "55.00M".
    // Values below 1000 are returned as a plain "%.2f" with no suffix.
    void display_format_abbreviate(double value, char *out, size_t out_len);

    // Maps `value` from the domain [lo, hi] onto the integer range
    // [0, scale_max], clamped. Used to feed fixed-point LVGL chart series/axis
    // APIs from raw double price data without the precision loss of a flat
    // fixed-point scale (e.g. a "* 100" cents conversion, which collapses all
    // sub-cent price variation for low-priced symbols into a single integer and
    // makes the chart line look flat or jitter between 0/1 like an oscilloscope).
    // If the range is degenerate (hi <= lo), returns scale_max / 2 for every
    // value so the series renders as a flat, centered line instead.
    int32_t display_format_normalize_value(double value, double lo, double hi, int32_t scale_max);

#ifdef __cplusplus
}
#endif
