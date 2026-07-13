#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure helpers shared by the LVGL UI and host-side tests. Keep ESP-IDF and
// LVGL types out of this interface so the behavior can be verified on host.

void display_ui_logic_format_relative_time(int64_t event_ms, int64_t now_ms, char *out, size_t out_len);

uint8_t display_ui_logic_rssi_to_bars(int8_t rssi);

size_t display_ui_logic_sanitize_symbol(const char *raw, char *out, size_t out_cap);

bool display_ui_logic_minute_in_window(uint16_t current_minute, uint16_t start_minute, uint16_t end_minute);
