#include "display_ui_logic.h"

#include <inttypes.h>
#include <stdio.h>

#define MINUTES_PER_DAY 1440U

void display_ui_logic_format_relative_time(int64_t event_ms, int64_t now_ms, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U)
    {
        return;
    }
    if (event_ms == 0)
    {
        snprintf(out, out_len, "Never");
        return;
    }

    int64_t delta_s = (now_ms - event_ms) / 1000;
    if (delta_s < 60)
    {
        snprintf(out, out_len, "Just now");
    }
    else if (delta_s < 3600)
    {
        snprintf(out, out_len, "%" PRId64 " min ago", delta_s / 60);
    }
    else if (delta_s < 86400)
    {
        snprintf(out, out_len, "%" PRId64 " h ago", delta_s / 3600);
    }
    else
    {
        snprintf(out, out_len, "%" PRId64 " d ago", delta_s / 86400);
    }
}

uint8_t display_ui_logic_rssi_to_bars(int8_t rssi)
{
    if (rssi >= -55)
    {
        return 4;
    }
    if (rssi >= -67)
    {
        return 3;
    }
    if (rssi >= -78)
    {
        return 2;
    }
    return 1;
}

size_t display_ui_logic_sanitize_symbol(const char *raw, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0U)
    {
        return 0U;
    }
    out[0] = '\0';
    if (raw == NULL)
    {
        return 0U;
    }

    size_t n = 0U;
    for (const char *p = raw; *p != '\0' && n + 1U < out_cap; p++)
    {
        unsigned char ch = (unsigned char)*p;
        if (ch >= (unsigned char)'a' && ch <= (unsigned char)'z')
        {
            ch = (unsigned char)(ch - (unsigned char)'a' + (unsigned char)'A');
        }
        if ((ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') ||
            (ch >= (unsigned char)'0' && ch <= (unsigned char)'9'))
        {
            out[n++] = (char)ch;
        }
    }
    out[n] = '\0';
    return n;
}

bool display_ui_logic_minute_in_window(uint16_t current_minute, uint16_t start_minute, uint16_t end_minute)
{
    if (current_minute >= MINUTES_PER_DAY || start_minute >= MINUTES_PER_DAY || end_minute >= MINUTES_PER_DAY)
    {
        return false;
    }
    if (start_minute <= end_minute)
    {
        return current_minute >= start_minute && current_minute < end_minute;
    }
    return current_minute >= start_minute || current_minute < end_minute;
}
