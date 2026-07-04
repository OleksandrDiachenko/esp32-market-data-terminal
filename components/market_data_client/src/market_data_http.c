#include "market_data_http.h"

#include <errno.h>
#include <stdlib.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

// Small enough to keep off the stack of a task with a modest stack size,
// large enough to keep syscall overhead reasonable. No PSRAM needed - this
// is the only per-request buffer left after streaming parsing removed the
// need to hold a full response body.
#define MARKET_DATA_HTTP_CHUNK_SIZE 1024

static const char *TAG = "market_data_http";

struct market_data_http_session
{
    esp_http_client_handle_t client;
};

static market_data_err_t map_open_error(esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT || errno == ETIMEDOUT)
    {
        return MARKET_DATA_ERR_TIMEOUT;
    }
    return MARKET_DATA_ERR_NETWORK;
}

market_data_err_t market_data_http_open(const char *url, uint32_t timeout_ms, market_data_http_session_t **out_session,
                                         int *out_status)
{
    if (url == NULL || out_session == NULL || out_status == NULL)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return MARKET_DATA_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Open failed for %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return map_open_error(err);
    }

    // A chunked-encoding response reports content_length == -1 here; that's
    // not itself an error, esp_http_client_read() handles chunked and
    // fixed-length bodies transparently, so the return value is discarded.
    (void)esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status <= 0)
    {
        ESP_LOGW(TAG, "Fetching headers failed for %s", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return MARKET_DATA_ERR_NETWORK;
    }

    market_data_http_session_t *session = malloc(sizeof(*session));
    if (session == NULL)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return MARKET_DATA_ERR_NO_MEM;
    }
    session->client = client;
    *out_session = session;
    *out_status = status;
    return MARKET_DATA_OK;
}

market_data_err_t market_data_http_next(market_data_http_session_t *session, const char *url, uint32_t timeout_ms,
                                         int *out_status)
{
    if (session == NULL || url == NULL || out_status == NULL)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    // Tears down the previous request's transport connection but keeps the
    // handle (headers, transport list, TLS session state) alive for reuse.
    esp_http_client_close(session->client);
    esp_http_client_clear_response_buffer(session->client);

    if (esp_http_client_set_url(session->client, url) != ESP_OK)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    esp_http_client_set_timeout_ms(session->client, (int)timeout_ms);

    esp_err_t err = esp_http_client_open(session->client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Open failed for %s: %s", url, esp_err_to_name(err));
        return map_open_error(err);
    }

    (void)esp_http_client_fetch_headers(session->client);

    int status = esp_http_client_get_status_code(session->client);
    if (status <= 0)
    {
        ESP_LOGW(TAG, "Fetching headers failed for %s", url);
        return MARKET_DATA_ERR_NETWORK;
    }

    *out_status = status;
    return MARKET_DATA_OK;
}

market_data_err_t market_data_http_stream_body(market_data_http_session_t *session, market_data_http_body_sink_t sink,
                                                void *sink_ctx)
{
    if (session == NULL || sink == NULL)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    char chunk[MARKET_DATA_HTTP_CHUNK_SIZE];
    for (;;)
    {
        int n = esp_http_client_read(session->client, chunk, sizeof(chunk));
        if (n < 0)
        {
            return (errno == ETIMEDOUT) ? MARKET_DATA_ERR_TIMEOUT : MARKET_DATA_ERR_NETWORK;
        }
        if (n == 0)
        {
            return MARKET_DATA_OK;
        }
        market_data_err_t err = sink(sink_ctx, chunk, (size_t)n);
        if (err != MARKET_DATA_OK)
        {
            return err;
        }
    }
}

market_data_err_t market_data_http_read_body_snippet(market_data_http_session_t *session, char *out,
                                                       size_t out_capacity, size_t *out_len)
{
    if (session == NULL || out == NULL || out_capacity == 0)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    size_t total = 0;
    while (total + 1 < out_capacity)
    {
        int n = esp_http_client_read(session->client, out + total, out_capacity - 1 - total);
        if (n < 0)
        {
            return (errno == ETIMEDOUT) ? MARKET_DATA_ERR_TIMEOUT : MARKET_DATA_ERR_NETWORK;
        }
        if (n == 0)
        {
            break;
        }
        total += (size_t)n;
    }
    out[total] = '\0';
    if (out_len != NULL)
    {
        *out_len = total;
    }
    return MARKET_DATA_OK;
}

void market_data_http_close(market_data_http_session_t *session)
{
    if (session == NULL)
    {
        return;
    }
    esp_http_client_close(session->client);
    esp_http_client_cleanup(session->client);
    free(session);
}
