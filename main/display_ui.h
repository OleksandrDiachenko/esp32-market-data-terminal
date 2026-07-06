#pragma once

#include "esp_err.h"

/**
 * Brings up the ST7701S display and GT911 touch, and renders the watchlist
 * screen (one row per app_state symbol, updated in place from a periodic
 * LVGL timer). Returns an error if display or touch bring-up fails.
 */
esp_err_t display_ui_start(void);

/**
 * Registers a dev-only "nav" console command that jumps straight to a named
 * screen (watchlist | settings | wifi | wifi_password [ssid]) without a
 * physical tap, for driving tools/dev_screenshot.py --nav. Shares
 * CONFIG_DEV_SCREENSHOT_CONSOLE with dev_screenshot_console.c - a no-op
 * returning ESP_OK unless that flag is enabled locally via menuconfig.
 * Call once, after ota_console_start() (needs the same console REPL).
 */
esp_err_t display_ui_register_dev_nav_console(void);
