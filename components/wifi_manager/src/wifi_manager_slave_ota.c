#include "wifi_manager_slave_ota.h"

#include <inttypes.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_manager_slave_ota";

#define SLAVE_FW_PARTITION_LABEL "slave_fw"
#define SLAVE_OTA_CHUNK_SIZE 1500
#define SLAVE_OTA_RESTART_DELAY_MS 2000

static bool partition_is_empty(const esp_partition_t *partition)
{
    uint8_t buffer[256];
    size_t checked = 0;

    while (checked < 1024 && checked < partition->size)
    {
        size_t chunk = (1024 - checked > sizeof(buffer)) ? sizeof(buffer) : (1024 - checked);
        if (esp_partition_read(partition, checked, buffer, chunk) != ESP_OK)
        {
            // Treat an unreadable partition the same as an empty one - no
            // firmware to push, not an error worth failing Wi-Fi bring-up
            // over.
            return true;
        }
        for (size_t i = 0; i < chunk; i++)
        {
            if (buffer[i] != 0xFF)
            {
                return false;
            }
        }
        checked += chunk;
    }
    return true;
}

static esp_err_t parse_embedded_image(const esp_partition_t *partition, size_t *out_size,
                                       char *out_version, size_t out_version_len)
{
    esp_image_header_t header;
    if (esp_partition_read(partition, 0, &header, sizeof(header)) != ESP_OK)
    {
        return ESP_FAIL;
    }
    if (header.magic != ESP_IMAGE_HEADER_MAGIC)
    {
        ESP_LOGE(TAG, "'%s' does not contain a valid app image (magic 0x%02x)", partition->label,
                 header.magic);
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = sizeof(header);
    size_t total = sizeof(header);
    esp_app_desc_t app_desc = {0};
    bool have_app_desc = false;

    for (int i = 0; i < header.segment_count; i++)
    {
        esp_image_segment_header_t segment;
        if (esp_partition_read(partition, offset, &segment, sizeof(segment)) != ESP_OK)
        {
            return ESP_FAIL;
        }
        if (i == 0 && esp_partition_read(partition, offset + sizeof(segment), &app_desc, sizeof(app_desc)) == ESP_OK)
        {
            have_app_desc = true;
        }
        total += sizeof(segment) + segment.data_len;
        offset += sizeof(segment) + segment.data_len;
    }

    // The 1-byte checksum is itself part of what gets padded to a 16-byte
    // boundary (not appended after padding) - the SHA-256, if present,
    // follows that padded block.
    total += 1; // checksum byte
    size_t padding = (16 - (total % 16)) % 16;
    total += padding;
    if (header.hash_appended)
    {
        total += 32; // SHA-256
    }

    *out_size = total;
    if (have_app_desc)
    {
        strncpy(out_version, app_desc.version, out_version_len - 1);
        out_version[out_version_len - 1] = '\0';
    }
    else
    {
        strncpy(out_version, "unknown", out_version_len - 1);
        out_version[out_version_len - 1] = '\0';
    }
    return ESP_OK;
}

static bool coprocessor_version_matches(const char *embedded_version)
{
    esp_hosted_coprocessor_fwver_t running = {0};
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 5 && ret != ESP_OK; attempt++)
    {
        ret = esp_hosted_get_coprocessor_fwversion(&running);
        if (ret != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not read running co-processor version; assuming update is needed");
        return false;
    }

    char running_str[32];
    snprintf(running_str, sizeof(running_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, running.major1,
             running.minor1, running.patch1);

    ESP_LOGI(TAG, "Co-processor firmware: running=%s embedded=%s", running_str, embedded_version);
    return strcmp(running_str, embedded_version) == 0;
}

static esp_err_t push_image_to_slave(const esp_partition_t *partition, size_t image_size)
{
    esp_err_t ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t chunk[SLAVE_OTA_CHUNK_SIZE];
    size_t offset = 0;
    while (offset < image_size)
    {
        size_t to_read = (image_size - offset > sizeof(chunk)) ? sizeof(chunk) : (image_size - offset);
        ret = esp_partition_read(partition, offset, chunk, to_read);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Reading '%s' at offset %u failed: %s", partition->label, (unsigned)offset,
                     esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return ret;
        }
        ret = esp_hosted_slave_ota_write(chunk, to_read);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_hosted_slave_ota_write failed at offset %u: %s", (unsigned)offset,
                     esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return ret;
        }
        offset += to_read;
    }

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_end failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pushed %u bytes of co-processor firmware", (unsigned)image_size);
    return ESP_OK;
}

esp_err_t wifi_manager_slave_ota_check_and_update(void)
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, SLAVE_FW_PARTITION_LABEL);
    if (partition == NULL)
    {
        ESP_LOGI(TAG, "No '%s' partition - skipping co-processor firmware check", SLAVE_FW_PARTITION_LABEL);
        return ESP_OK;
    }
    if (partition_is_empty(partition))
    {
        ESP_LOGI(TAG, "'%s' partition is empty - skipping co-processor firmware check",
                 SLAVE_FW_PARTITION_LABEL);
        return ESP_OK;
    }

    size_t image_size = 0;
    char embedded_version[32];
    if (parse_embedded_image(partition, &image_size, embedded_version, sizeof(embedded_version)) != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not parse embedded co-processor image - skipping firmware check");
        return ESP_OK;
    }

    if (coprocessor_version_matches(embedded_version))
    {
        ESP_LOGI(TAG, "Co-processor firmware already up to date");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Co-processor firmware differs from embedded %s - pushing update over SDIO",
             embedded_version);
    if (push_image_to_slave(partition, image_size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Co-processor firmware update failed - continuing with the currently running version");
        return ESP_OK;
    }

    esp_err_t ret = esp_hosted_slave_ota_activate();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_activate failed: %s - co-processor still holds the new image "
                      "but hasn't switched to it",
                 esp_err_to_name(ret));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Co-processor firmware activated - restarting host to resync");
    vTaskDelay(pdMS_TO_TICKS(SLAVE_OTA_RESTART_DELAY_MS));
    esp_restart();
    return ESP_OK; // unreachable
}
