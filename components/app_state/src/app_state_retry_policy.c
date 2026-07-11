#include "app_state_retry_policy.h"

bool app_state_retry_is_recoverable(market_data_err_t err)
{
    switch (err)
    {
    case MARKET_DATA_ERR_NETWORK:
    case MARKET_DATA_ERR_TIMEOUT:
    case MARKET_DATA_ERR_HTTP_STATUS:
    case MARKET_DATA_ERR_RATE_LIMITED:
    case MARKET_DATA_ERR_NOT_SYNCED:
    case MARKET_DATA_ERR_PARSE:
    case MARKET_DATA_ERR_NO_MEM:
        return true;
    case MARKET_DATA_OK:
    case MARKET_DATA_ERR_INVALID_ARG:
    case MARKET_DATA_ERR_SYMBOL_NOT_FOUND:
    default:
        return false;
    }
}

bool app_state_retry_invalid_symbol_is_recoverable(uint8_t prior_strikes)
{
    return (uint32_t)prior_strikes + 1 < APP_STATE_MAX_INVALID_SYMBOL_ATTEMPTS;
}

uint32_t app_state_retry_backoff_delay_ms(uint32_t base_ms, uint32_t max_ms, uint8_t attempt)
{
    if (max_ms < base_ms)
    {
        max_ms = base_ms;
    }
    uint64_t delay = base_ms;
    for (uint8_t i = 0; i < attempt && delay < max_ms; i++)
    {
        delay *= 2;
    }
    if (delay > max_ms)
    {
        delay = max_ms;
    }
    return (uint32_t)delay;
}

bool app_state_retry_needs_resync(uint32_t disconnected_ms, uint32_t interval_ms)
{
    return disconnected_ms >= interval_ms;
}
