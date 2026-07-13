#include "market_data_ws_control.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "settings_codec.h" // SETTINGS_SYMBOL_MAX_LEN

market_data_err_t market_data_ws_build_control_message(const char *method, const char *symbol,
                                                       const char *stream_suffix, uint32_t id, char *out,
                                                       size_t out_capacity)
{
    if (method == NULL || method[0] == '\0' || symbol == NULL || stream_suffix == NULL || stream_suffix[0] == '\0' ||
        out == NULL || out_capacity == 0)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    size_t symbol_len = strlen(symbol);
    if (symbol_len == 0 || symbol_len > SETTINGS_SYMBOL_MAX_LEN)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }

    char lower_symbol[SETTINGS_SYMBOL_MAX_LEN + 1];
    for (size_t i = 0; i < symbol_len; i++)
    {
        if (!isalnum((unsigned char)symbol[i]))
        {
            return MARKET_DATA_ERR_INVALID_ARG;
        }
        lower_symbol[i] = (char)tolower((unsigned char)symbol[i]);
    }
    lower_symbol[symbol_len] = '\0';

    int written = snprintf(out, out_capacity, "{\"method\":\"%s\",\"params\":[\"%s@%s\"],\"id\":%u}", method,
                           lower_symbol, stream_suffix, (unsigned int)id);
    if (written < 0 || (size_t)written >= out_capacity)
    {
        return MARKET_DATA_ERR_INVALID_ARG;
    }
    return MARKET_DATA_OK;
}
