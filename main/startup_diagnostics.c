#include "startup_diagnostics.h"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

// Conservative startup guard; adjust after display and network bring-up measurements.
#define INTERNAL_HEAP_EXPECTED_MINIMUM (100U * 1024U)
#define CHIP_MODEL_EXPECTED CHIP_ESP32P4

static const char *TAG = "startup_diag";

static const char *startup_diagnostics_chip_model_name(esp_chip_model_t model);
static esp_err_t startup_diagnostics_chip_model(esp_chip_model_t expected_model, esp_chip_model_t chip_model);
static esp_err_t startup_diagnostics_check_internal_heap(uint32_t expected_minimum, uint32_t *out_free_heap);

esp_err_t startup_diagnostics(void)
{
    ESP_LOGI(TAG, "Performing startup diagnostics...");

    esp_chip_info_t chip_info = {0};
    esp_chip_info(&chip_info);

    if (startup_diagnostics_chip_model(CHIP_MODEL_EXPECTED, chip_info.model) != ESP_OK)
    {
        ESP_LOGE(TAG, "Chip model diagnostics failed, actual: %s",
                 startup_diagnostics_chip_model_name(chip_info.model));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Chip model diagnostics passed, model: %s", startup_diagnostics_chip_model_name(chip_info.model));
    ESP_LOGI(TAG, "Chip revision: %d", chip_info.revision);
    ESP_LOGI(TAG, "Chip features: 0x%08X", chip_info.features);
    ESP_LOGI(TAG, "Chip cores: %d", chip_info.cores);

    uint32_t internal_free_heap = 0;
    if (startup_diagnostics_check_internal_heap(INTERNAL_HEAP_EXPECTED_MINIMUM, &internal_free_heap) != ESP_OK)
    {
        ESP_LOGE(TAG, "Internal heap diagnostics failed, expected minimum: %u, actual: %u",
                 INTERNAL_HEAP_EXPECTED_MINIMUM, internal_free_heap);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Internal heap diagnostics passed, free heap: %u", internal_free_heap);

    ESP_LOGI(TAG, "Startup diagnostics completed.");
    return ESP_OK;
}

static esp_err_t startup_diagnostics_chip_model(esp_chip_model_t expected_model, esp_chip_model_t chip_model)
{
    if (chip_model == expected_model)
    {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static esp_err_t startup_diagnostics_check_internal_heap(uint32_t expected_minimum, uint32_t *out_free_heap)
{
    if (out_free_heap == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (expected_minimum == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (*out_free_heap < expected_minimum)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static const char *startup_diagnostics_chip_model_name(esp_chip_model_t model)
{
    switch (model)
    {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S2:
        return "ESP32-S2";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C3:
        return "ESP32-C3";
    case CHIP_ESP32C2:
        return "ESP32-C2";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    case CHIP_ESP32H2:
        return "ESP32-H2";
    case CHIP_ESP32P4:
        return "ESP32-P4";
    case CHIP_ESP32C61:
        return "ESP32-C61";
    case CHIP_ESP32C5:
        return "ESP32-C5";
    case CHIP_ESP32H21:
        return "ESP32-H21";
    case CHIP_ESP32H4:
        return "ESP32-H4";
    case CHIP_POSIX_LINUX:
        return "POSIX/Linux simulator";
    default:
        return "Unknown chip model";
    }
}
