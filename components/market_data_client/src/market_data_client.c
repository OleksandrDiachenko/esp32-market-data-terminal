#include "market_data_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "market_data_http.h"
#include "market_data_klines_parser.h"
#include "market_data_symbol_parser.h"
#include "market_data_url.h"
#include "settings_store.h"
#include "time_sync.h"

#define MARKET_DATA_HTTP_TIMEOUT_MS 10000
#define MARKET_DATA_ERROR_BODY_MAX 256
#define MARKET_DATA_URL_MAX 256

static const char *TAG = "market_data_client";

// Re-read on every call rather than cached: this is a fast local NVS blob
// read, and caching would risk a stale base URL if the user changes the
// region setting without a reboot.
static const char *select_base_url(void)
{
    api_region_settings_t region;
    settings_store_load_api_region(&region); // always ESP_OK, defaults substituted on corruption
    return region.region == SETTINGS_API_REGION_US ? "https://api.binance.us" : "https://api.binance.com";
}

static market_data_err_t status_to_generic_error(int status)
{
    if (status == 429 || status == 418)
    {
        return MARKET_DATA_ERR_RATE_LIMITED;
    }
    return MARKET_DATA_ERR_HTTP_STATUS;
}

// Binance error bodies are tiny, fixed-shape {"code":-1121,"msg":"..."}
// objects - a substring search for the exact "code":<n> token is enough
// here and avoids pulling the streaming parser (built for the two
// *success*-path shapes) into a third grammar for one diagnostic field.
static bool contains_binance_error_code(const char *body, int32_t code)
{
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"code\":%d", (int)code);
    if (n <= 0 || (size_t)n >= sizeof(needle))
    {
        return false;
    }
    return strstr(body, needle) != NULL;
}

static market_data_err_t symbol_sink(void *ctx, const char *chunk, size_t len)
{
    return market_data_symbol_parser_feed((market_data_symbol_parser_t *)ctx, chunk, len);
}

static market_data_err_t klines_sink(void *ctx, const char *chunk, size_t len)
{
    return market_data_klines_parser_feed((market_data_klines_parser_t *)ctx, chunk, len);
}

market_data_err_t market_data_client_fetch_symbol_status(const char *symbol, market_data_symbol_status_t *out_status)
{
    if (symbol == NULL || out_status == NULL)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (!time_sync_is_synced())
    {
        return MARKET_DATA_ERR_NOT_SYNCED;
    }

    char url[MARKET_DATA_URL_MAX];
    market_data_err_t err = market_data_url_build_exchange_info(select_base_url(), symbol, url, sizeof(url));
    if (err != MARKET_DATA_OK)
    {
        return err;
    }

    market_data_http_session_t *session = NULL;
    int status = 0;
    err = market_data_http_open(url, MARKET_DATA_HTTP_TIMEOUT_MS, &session, &status);
    if (err != MARKET_DATA_OK)
    {
        return err;
    }

    if (status == 200)
    {
        market_data_symbol_parser_t parser;
        market_data_symbol_parser_init(&parser, out_status);
        err = market_data_http_stream_body(session, symbol_sink, &parser);
        market_data_http_close(session);
        if (err != MARKET_DATA_OK)
        {
            return err;
        }
        return market_data_symbol_parser_finish(&parser);
    }

    char body[MARKET_DATA_ERROR_BODY_MAX];
    size_t body_len = 0;
    (void)market_data_http_read_body_snippet(session, body, sizeof(body), &body_len);
    market_data_http_close(session);

    if (status == 400 && contains_binance_error_code(body, -1121))
    {
        return MARKET_DATA_ERR_SYMBOL_NOT_FOUND;
    }
    ESP_LOGW(TAG, "exchangeInfo request for '%s' failed: HTTP %d: %.*s", symbol, status, (int)body_len, body);
    return status_to_generic_error(status);
}

market_data_err_t market_data_client_fetch_klines(const market_data_klines_request_t *req,
                                                    market_data_kline_t *out_klines, uint16_t out_capacity,
                                                    uint16_t *out_count)
{
    if (req == NULL || out_klines == NULL || out_count == NULL || req->symbol == NULL || req->interval == NULL)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (!time_sync_is_synced())
    {
        return MARKET_DATA_ERR_NOT_SYNCED;
    }

    char url[MARKET_DATA_URL_MAX];
    market_data_err_t err = market_data_url_build_klines(select_base_url(), req, url, sizeof(url));
    if (err != MARKET_DATA_OK)
    {
        return err;
    }

    market_data_http_session_t *session = NULL;
    int status = 0;
    err = market_data_http_open(url, MARKET_DATA_HTTP_TIMEOUT_MS, &session, &status);
    if (err != MARKET_DATA_OK)
    {
        return err;
    }

    if (status == 200)
    {
        market_data_klines_parser_t parser;
        market_data_klines_parser_init(&parser, out_klines, out_capacity);
        err = market_data_http_stream_body(session, klines_sink, &parser);
        market_data_http_close(session);
        if (err != MARKET_DATA_OK)
        {
            return err;
        }
        return market_data_klines_parser_finish(&parser, out_count);
    }

    char body[MARKET_DATA_ERROR_BODY_MAX];
    size_t body_len = 0;
    (void)market_data_http_read_body_snippet(session, body, sizeof(body), &body_len);
    market_data_http_close(session);

    ESP_LOGW(TAG, "klines request for '%s' failed: HTTP %d: %.*s", req->symbol, status, (int)body_len, body);
    return status_to_generic_error(status);
}

market_data_err_t market_data_client_fetch_klines_24h_5m(const char *symbol, market_data_kline_t *out_klines,
                                                           uint16_t out_capacity, uint16_t *out_count)
{
    market_data_klines_request_t req = {
        .symbol = symbol,
        .interval = MARKET_DATA_KLINE_INTERVAL_5M,
        .limit = MARKET_DATA_KLINES_V1_LIMIT,
        .start_time_ms = 0,
        .end_time_ms = 0,
    };
    return market_data_client_fetch_klines(&req, out_klines, out_capacity, out_count);
}

market_data_err_t market_data_client_fetch_klines_24h_5m_batch(const char *const *symbols, uint8_t symbol_count,
                                                                 market_data_kline_t *const *out_klines_per_symbol,
                                                                 uint16_t out_capacity_per_symbol,
                                                                 market_data_batch_result_t *out_results)
{
    if (symbols == NULL || symbol_count == 0 || out_klines_per_symbol == NULL || out_capacity_per_symbol == 0 ||
        out_results == NULL)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < symbol_count; i++)
    {
        if (symbols[i] == NULL || out_klines_per_symbol[i] == NULL)
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
    }

    if (!time_sync_is_synced())
    {
        return MARKET_DATA_ERR_NOT_SYNCED;
    }

    const char *base_url = select_base_url();
    market_data_http_session_t *session = NULL;

    for (uint8_t i = 0; i < symbol_count; i++)
    {
        out_results[i].symbol = symbols[i];
        out_results[i].err = MARKET_DATA_OK;
        out_results[i].count = 0;

        market_data_klines_request_t req = {
            .symbol = symbols[i],
            .interval = MARKET_DATA_KLINE_INTERVAL_5M,
            .limit = MARKET_DATA_KLINES_V1_LIMIT,
            .start_time_ms = 0,
            .end_time_ms = 0,
        };
        char url[MARKET_DATA_URL_MAX];
        market_data_err_t err = market_data_url_build_klines(base_url, &req, url, sizeof(url));
        if (err != MARKET_DATA_OK)
        {
            out_results[i].err = err;
            continue;
        }

        int status = 0;
        // session == NULL only before the first successful open() - once
        // set, it stays valid (and reused via market_data_http_next()) for
        // the rest of the batch even if individual requests fail.
        err = (session == NULL) ? market_data_http_open(url, MARKET_DATA_HTTP_TIMEOUT_MS, &session, &status)
                                 : market_data_http_next(session, url, MARKET_DATA_HTTP_TIMEOUT_MS, &status);
        if (err != MARKET_DATA_OK)
        {
            out_results[i].err = err;
            continue;
        }

        if (status == 200)
        {
            market_data_klines_parser_t parser;
            market_data_klines_parser_init(&parser, out_klines_per_symbol[i], out_capacity_per_symbol);
            err = market_data_http_stream_body(session, klines_sink, &parser);
            if (err == MARKET_DATA_OK)
            {
                err = market_data_klines_parser_finish(&parser, &out_results[i].count);
            }
            out_results[i].err = err;
            continue;
        }

        char body[MARKET_DATA_ERROR_BODY_MAX];
        size_t body_len = 0;
        (void)market_data_http_read_body_snippet(session, body, sizeof(body), &body_len);
        ESP_LOGW(TAG, "klines request for '%s' failed: HTTP %d: %.*s", symbols[i], status, (int)body_len, body);
        out_results[i].err = status_to_generic_error(status);
    }

    if (session != NULL)
    {
        market_data_http_close(session);
    }
    return MARKET_DATA_OK;
}
