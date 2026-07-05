#pragma once

#include "esp_err.h"

/**
 * Brings up the ST7701S display and GT911 touch, and renders the watchlist
 * screen (one row per app_state symbol, updated in place from a periodic
 * LVGL timer). Returns an error if display or touch bring-up fails.
 */
esp_err_t display_ui_start(void);
