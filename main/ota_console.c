#include "ota_console.h"

#include "app_state_ota_task.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "ota_console";

static int cmd_ota_check(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_state_ota_check_now();
    printf("OTA check requested - see logs for the result.\n");
    return 0;
}

static int cmd_ota_update(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_state_ota_update_now();
    printf("OTA update requested (only runs if a check found one available) - device reboots on success.\n");
    return 0;
}

esp_err_t ota_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ota>";

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create console REPL: %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t ota_check_cmd = {
        .command = "ota_check",
        .help = "Check GitHub Releases for a new firmware version now",
        .hint = NULL,
        .func = &cmd_ota_check,
    };
    const esp_console_cmd_t ota_update_cmd = {
        .command = "ota_update",
        .help = "Flash the latest release if a check has found one available",
        .hint = NULL,
        .func = &cmd_ota_update,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ota_check_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&ota_update_cmd));

    return esp_console_start_repl(repl);
}
