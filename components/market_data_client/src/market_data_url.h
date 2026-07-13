#pragma once

// Pure C, host-compilable: builds Binance REST request URLs. Symbols and
// intervals are restricted to a conservative charset (alnum only) rather
// than percent-encoded - Binance never issues symbols/intervals outside
// that charset, so this is validation, not an escaping scheme.

#include <stddef.h>
#include <stdint.h>

#include "market_data_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Writes "<base_url>/api/v3/exchangeInfo?symbol=<symbol>" into out (a
    // caller-owned buffer of out_capacity bytes, NUL-terminated on success).
    market_data_err_t market_data_url_build_exchange_info(const char *base_url, const char *symbol, char *out,
                                                          size_t out_capacity);

    // Writes "<base_url>/api/v3/klines?symbol=...&interval=...[&limit=...]
    // [&startTime=...][&endTime=...]" into out. limit/startTime/endTime are
    // omitted when the request struct leaves them at 0.
    market_data_err_t market_data_url_build_klines(const char *base_url, const market_data_klines_request_t *req,
                                                   char *out, size_t out_capacity);

    // Writes "<base_url>/api/v3/ticker/24hr?symbol=<symbol>" into out.
    market_data_err_t market_data_url_build_ticker_24hr(const char *base_url, const char *symbol, char *out,
                                                        size_t out_capacity);

#ifdef __cplusplus
}
#endif
