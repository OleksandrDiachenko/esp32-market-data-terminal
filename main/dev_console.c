#include "dev_console.h"

#include "sdkconfig.h"

#if CONFIG_DEV_SCREENSHOT_CONSOLE

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "dev_console";

esp_err_t dev_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "dev>";
    // lv_snapshot_take_to_draw_buf() (main/dev_screenshot_console.c) runs
    // LVGL's full render pipeline synchronously on this REPL task's own
    // stack - the same recursive obj_refr/draw_rect/border-drawing chain
    // the LVGL port's own render task runs with a 7168-byte stack
    // (ESP_LVGL_PORT_INIT_CONFIG() in esp_lvgl_port.h). The default
    // 4096-byte REPL stack overflowed here on real hardware ("Stack
    // protection fault" in task "console_repl").
    repl_config.task_stack_size = 12288;

    esp_console_dev_usb_serial_jtag_config_t jtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&jtag_config, &repl_config, &repl);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create console REPL: %s", esp_err_to_name(err));
        return err;
    }

    return esp_console_start_repl(repl);
}

#else // !CONFIG_DEV_SCREENSHOT_CONSOLE

esp_err_t dev_console_start(void)
{
    return ESP_OK;
}

#endif
