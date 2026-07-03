#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_JC4880P443C_LCD_H_RES 480
#define BOARD_JC4880P443C_LCD_V_RES 800

/**
 * Initializes the ST7701S panel over MIPI DSI and registers it with LVGL.
 * Does not touch touch/audio/storage peripherals.
 */
esp_err_t board_jc4880p443c_display_start(lv_display_t **out_display);

/**
 * Initializes the GT911 touch controller and registers it as an LVGL input
 * device for the given display. Call after board_jc4880p443c_display_start().
 */
esp_err_t board_jc4880p443c_touch_start(lv_display_t *display, lv_indev_t **out_indev);

esp_err_t board_jc4880p443c_backlight_on(void);
esp_err_t board_jc4880p443c_backlight_off(void);

bool board_jc4880p443c_display_lock(uint32_t timeout_ms);
void board_jc4880p443c_display_unlock(void);

#ifdef __cplusplus
}
#endif
