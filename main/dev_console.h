#pragma once

// Creates and starts the console REPL (USB-Serial-JTAG) used by the dev-only
// screenshot/nav commands (dev_screenshot_console.c,
// display_ui_register_dev_nav_console()). No-op returning ESP_OK unless
// CONFIG_DEV_SCREENSHOT_CONSOLE is enabled locally via menuconfig - never
// set that flag in sdkconfig.defaults; there are no other console commands
// left to serve in a production build.

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once, before dev_screenshot_console_register() /
// display_ui_register_dev_nav_console() (they register into this REPL).
esp_err_t dev_console_start(void);

#ifdef __cplusplus
}
#endif
