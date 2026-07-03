#pragma once

#include "esp_err.h"

/**
 * Brings up the ST7701S display and GT911 touch, and renders the minimal
 * skeleton UI. Returns an error if display or touch bring-up fails.
 */
esp_err_t display_ui_start(void);
