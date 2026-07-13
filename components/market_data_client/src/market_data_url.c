#include "market_data_url.h"

#include <ctype.h>
#include <stdio.h>

static bool is_alnum_str(const char *s)
{
    if (s == NULL || s[0] == '\0')
    {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++)
    {
        if (!isalnum((unsigned char)*p))
        {
            return false;
        }
    }
    return true;
}

market_data_err_t market_data_url_build_exchange_info(const char *base_url, const char *symbol, char *out,
                                                      size_t out_capacity)
{
    if (base_url == NULL || out == NULL || out_capacity == 0)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (!is_alnum_str(symbol))
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    int written = snprintf(out, out_capacity, "%s/api/v3/exchangeInfo?symbol=%s", base_url, symbol);
    if (written < 0 || (size_t)written >= out_capacity)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    return MARKET_DATA_OK;
}

market_data_err_t market_data_url_build_ticker_24hr(const char *base_url, const char *symbol, char *out,
                                                    size_t out_capacity)
{
    if (base_url == NULL || out == NULL || out_capacity == 0)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (!is_alnum_str(symbol))
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    int written = snprintf(out, out_capacity, "%s/api/v3/ticker/24hr?symbol=%s", base_url, symbol);
    if (written < 0 || (size_t)written >= out_capacity)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    return MARKET_DATA_OK;
}

market_data_err_t market_data_url_build_klines(const char *base_url, const market_data_klines_request_t *req, char *out,
                                               size_t out_capacity)
{
    if (base_url == NULL || req == NULL || out == NULL || out_capacity == 0)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (!is_alnum_str(req->symbol) || !is_alnum_str(req->interval))
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (req->limit > MARKET_DATA_KLINES_MAX_LIMIT)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (req->start_time_ms < 0 || req->end_time_ms < 0)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    int written =
        snprintf(out, out_capacity, "%s/api/v3/klines?symbol=%s&interval=%s", base_url, req->symbol, req->interval);
    if (written < 0 || (size_t)written >= out_capacity)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    size_t len = (size_t)written;

    if (req->limit > 0)
    {
        written = snprintf(out + len, out_capacity - len, "&limit=%u", (unsigned int)req->limit);
        if (written < 0 || (size_t)written >= out_capacity - len)
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
        len += (size_t)written;
    }
    if (req->start_time_ms > 0)
    {
        written = snprintf(out + len, out_capacity - len, "&startTime=%lld", (long long)req->start_time_ms);
        if (written < 0 || (size_t)written >= out_capacity - len)
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
        len += (size_t)written;
    }
    if (req->end_time_ms > 0)
    {
        written = snprintf(out + len, out_capacity - len, "&endTime=%lld", (long long)req->end_time_ms);
        if (written < 0 || (size_t)written >= out_capacity - len)
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
    }
    return MARKET_DATA_OK;
}
