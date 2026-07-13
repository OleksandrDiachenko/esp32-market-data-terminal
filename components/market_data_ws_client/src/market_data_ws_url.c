#include "market_data_ws_url.h"

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

// Stream suffixes are Binance interval strings like "kline_1s" - alnum plus
// underscore, unlike ticker symbols which are pure alnum.
static bool is_valid_suffix(const char *s)
{
    if (s == NULL || s[0] == '\0')
    {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++)
    {
        if (!isalnum((unsigned char)*p) && *p != '_')
        {
            return false;
        }
    }
    return true;
}

// Appends src to out[0..out_capacity) at *len, advancing *len. Returns false
// (leaving *len unchanged) if src wouldn't fit.
static bool append(char *out, size_t out_capacity, size_t *len, const char *src)
{
    size_t src_len = 0;
    while (src[src_len] != '\0')
    {
        src_len++;
    }
    if (*len + src_len >= out_capacity) // room for src plus the final NUL
    {
        return false;
    }
    for (size_t i = 0; i < src_len; i++)
    {
        out[*len + i] = src[i];
    }
    *len += src_len;
    out[*len] = '\0';
    return true;
}

// Same as append(), but lowercases each byte of src (Binance stream names
// are lowercase; is_alnum_str() already guarantees plain ASCII alnum, so
// tolower() is safe here without locale surprises).
static bool append_lower(char *out, size_t out_capacity, size_t *len, const char *src)
{
    size_t src_len = 0;
    while (src[src_len] != '\0')
    {
        src_len++;
    }
    if (*len + src_len >= out_capacity)
    {
        return false;
    }
    for (size_t i = 0; i < src_len; i++)
    {
        out[*len + i] = (char)tolower((unsigned char)src[i]);
    }
    *len += src_len;
    out[*len] = '\0';
    return true;
}

market_data_err_t market_data_ws_url_build_combined_stream(const char *base_ws_url, const char *const *symbols,
                                                           uint8_t symbol_count, const char *stream_suffix, char *out,
                                                           size_t out_capacity)
{
    if (base_ws_url == NULL || symbols == NULL || symbol_count == 0 || stream_suffix == NULL || out == NULL ||
        out_capacity == 0)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    if (!is_valid_suffix(stream_suffix))
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < symbol_count; i++)
    {
        if (!is_alnum_str(symbols[i]))
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
    }

    size_t len = 0;
    out[0] = '\0';
    if (!append(out, out_capacity, &len, base_ws_url) || !append(out, out_capacity, &len, "/stream?streams="))
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < symbol_count; i++)
    {
        if (i > 0 && !append(out, out_capacity, &len, "/"))
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
        if (!append_lower(out, out_capacity, &len, symbols[i]) || !append(out, out_capacity, &len, "@") ||
            !append(out, out_capacity, &len, stream_suffix))
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
    }

    return MARKET_DATA_OK;
}
