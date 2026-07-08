#pragma once

// Registers a "screenshot" console command that captures the current LVGL
// screen into a PSRAM buffer and streams it back over the console as
// base64 text (see main/dev_screenshot_console.c for the wire format),
// for use by tools/dev_screenshot.py. Development-only: a no-op returning
// ESP_OK unless CONFIG_DEV_SCREENSHOT_CONSOLE is enabled locally via
// menuconfig - never set that flag in sdkconfig.defaults.

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once, after dev_console_start() (needs the same console REPL).
esp_err_t dev_screenshot_console_register(void);

#ifdef __cplusplus
}
#endif
