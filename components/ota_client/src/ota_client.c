#include "ota_client.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "ota_client_json.h"
#include "time_sync.h"

// GitHub requires a User-Agent header on API requests (403s without one).
#define OTA_CLIENT_USER_AGENT "esp32-market-data-terminal"
#define OTA_CLIENT_GITHUB_OWNER "OleksandrDiachenko"
#define OTA_CLIENT_GITHUB_REPO "esp32-market-data-terminal"
#define OTA_CLIENT_ASSET_NAME "esp32-market-data-terminal.bin"

#define OTA_CLIENT_HTTP_TIMEOUT_MS 10000
// GitHub's redirect Location header (release asset download -> a signed
// objects.githubusercontent.com URL) is far longer than esp_http_client's
// 512-byte default - see docs/decisions/0006-ota-firmware-update.md.
#define OTA_CLIENT_HTTP_BUFFER_SIZE 4096
// /releases/latest metadata is a few KB for this project's own releases;
// capped here rather than buffered without limit (see the design note in
// ota_client_json.c on why a bounded buffer + substring search is enough).
#define OTA_CLIENT_JSON_READ_CAP 4096

static const char *TAG = "ota_client";

static ota_client_err_t map_open_error(esp_err_t err)
{
    return (err == ESP_ERR_TIMEOUT) ? OTA_CLIENT_ERR_TIMEOUT : OTA_CLIENT_ERR_NETWORK;
}

ota_client_err_t ota_client_check_latest_release(ota_client_release_info_t *out_info)
{
    if (out_info == NULL)
    {
        return OTA_CLIENT_ERR_INVALID_ARG;
    }
    if (!time_sync_is_synced())
    {
        return OTA_CLIENT_ERR_NOT_SYNCED;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest", OTA_CLIENT_GITHUB_OWNER,
             OTA_CLIENT_GITHUB_REPO);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_CLIENT_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_CLIENT_HTTP_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return OTA_CLIENT_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "User-Agent", OTA_CLIENT_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Open failed for %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return map_open_error(err);
    }
    (void)esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status != 200)
    {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return OTA_CLIENT_ERR_HTTP_STATUS;
    }

    // Heap-allocated, not a function-static buffer: this call can now come
    // from either app_state_ota_task's background loop or the Settings >
    // Updates screen's "Check for updates" button (a different task), and a
    // shared static buffer would let those two calls race on the same
    // memory if they ever overlap.
    char *json_buf = malloc(OTA_CLIENT_JSON_READ_CAP);
    if (json_buf == NULL)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return OTA_CLIENT_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read_response(client, json_buf, OTA_CLIENT_JSON_READ_CAP - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (read_len < 0)
    {
        free(json_buf);
        return OTA_CLIENT_ERR_NETWORK;
    }
    json_buf[read_len] = '\0';

    ota_client_err_t parse_err =
        ota_client_extract_tag_name(json_buf, (size_t)read_len, out_info->tag_name, sizeof(out_info->tag_name));
    free(json_buf);
    if (parse_err != OTA_CLIENT_OK)
    {
        return parse_err;
    }

    const esp_app_desc_t *running_app_desc = esp_app_get_description();
    out_info->update_available = strcmp(out_info->tag_name, running_app_desc->version) != 0;
    return OTA_CLIENT_OK;
}

ota_client_err_t ota_client_update_to(const char *tag, ota_client_progress_cb_t progress_cb, void *progress_ctx)
{
    if (tag == NULL || tag[0] == '\0')
    {
        return OTA_CLIENT_ERR_INVALID_ARG;
    }
    if (!time_sync_is_synced())
    {
        return OTA_CLIENT_ERR_NOT_SYNCED;
    }

    // Well-known GitHub Releases direct-download URL for a named asset -
    // avoids re-parsing the release JSON's "assets" array for a
    // browser_download_url (see docs/decisions/0006-ota-firmware-update.md).
    // This itself redirects (302) to a signed objects.githubusercontent.com
    // URL, which is what OTA_CLIENT_HTTP_BUFFER_SIZE is sized for.
    char url[192];
    int written = snprintf(url, sizeof(url), "https://github.com/%s/%s/releases/download/%s/%s",
                           OTA_CLIENT_GITHUB_OWNER, OTA_CLIENT_GITHUB_REPO, tag, OTA_CLIENT_ASSET_NAME);
    if (written < 0 || (size_t)written >= sizeof(url))
    {
        return OTA_CLIENT_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA update to %s: %s", tag, url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = OTA_CLIENT_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_CLIENT_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_CLIENT_HTTP_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    // Streaming begin/perform/finish rather than the single-call
    // esp_https_ota() helper, so progress_cb can be driven off
    // esp_https_ota_get_image_len_read()/_get_image_size() between chunks -
    // this is what the Settings > Updates screen's progress bar reads.
    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return OTA_CLIENT_ERR_OTA_FAILED;
    }

    int total_size = esp_https_ota_get_image_size(https_ota_handle);
    do
    {
        err = esp_https_ota_perform(https_ota_handle);
        if (progress_cb != NULL)
        {
            int read_so_far = esp_https_ota_get_image_len_read(https_ota_handle);
            progress_cb((size_t)(read_so_far > 0 ? read_so_far : 0), (size_t)(total_size > 0 ? total_size : 0),
                        progress_ctx);
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(https_ota_handle))
    {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        return OTA_CLIENT_ERR_OTA_FAILED;
    }

    err = esp_https_ota_finish(https_ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return OTA_CLIENT_ERR_OTA_FAILED;
    }

    ESP_LOGI(TAG, "OTA succeeded, rebooting into %s", tag);
    esp_restart();
    // Unreachable - esp_restart() does not return.
    return OTA_CLIENT_OK;
}
