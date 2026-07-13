#pragma once

// Public API for the Binance public REST market data client. REST only (no
// WebSocket - see docs/decisions/0002-market-data-client.md), no API keys,
// no trading logic. This component has no _init()/_start(): it holds no
// long-lived state and runs no background task, so every call here is a
// plain blocking function invoked from the caller's own task. Periodic
// polling/orchestration is Phase 8's job, not this component's.
//
// Every fetch function requires time_sync_is_synced() to be true first
// (checked internally, not just documented) - an unsynced clock makes TLS
// certificate validation fail with a generic esp_tls error, so this is
// rejected up front with a specific, actionable error code instead.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        MARKET_DATA_OK = 0,
        MARKET_DATA_ERR_INVALID_ARG,
        MARKET_DATA_ERR_NOT_SYNCED, // time_sync_is_synced() == false; call not attempted
        MARKET_DATA_ERR_NETWORK,    // connect/TLS/socket failure
        MARKET_DATA_ERR_TIMEOUT,
        MARKET_DATA_ERR_HTTP_STATUS,      // non-2xx, not otherwise classified below
        MARKET_DATA_ERR_RATE_LIMITED,     // HTTP 429 or 418 (Binance IP-ban status)
        MARKET_DATA_ERR_SYMBOL_NOT_FOUND, // HTTP 400 + Binance error code -1121 "Invalid symbol"
        MARKET_DATA_ERR_PARSE,            // malformed/unexpected JSON shape
        MARKET_DATA_ERR_NO_MEM,
    } market_data_err_t;

    // --- exchangeInfo ---

    typedef struct
    {
        bool is_trading;          // symbols[0].status == "TRADING"
        bool has_spot_permission; // "SPOT" present in any inner permissionSets[] array
    } market_data_symbol_status_t;

    static inline bool market_data_symbol_is_usable(const market_data_symbol_status_t *s)
    {
        return s->is_trading && s->has_spot_permission;
    }

    // Fetches GET /api/v3/exchangeInfo?symbol=<symbol> and reports validity. On
    // MARKET_DATA_OK, *out_status is always populated - a symbol that
    // round-trips successfully but fails the usability checks still returns
    // MARKET_DATA_OK with one or both fields false. Callers must check
    // market_data_symbol_is_usable(), not the error code, to detect an
    // unusable-but-existing symbol. MARKET_DATA_ERR_SYMBOL_NOT_FOUND is reserved
    // for symbols Binance doesn't recognize at all.
    market_data_err_t market_data_client_fetch_symbol_status(const char *symbol,
                                                             market_data_symbol_status_t *out_status);

    // --- ticker/24hr ---

    typedef struct
    {
        double last_price;
        double price_change_percent;
        double high_price;
        double low_price;
    } market_data_ticker_24hr_t;

    // Fetches GET /api/v3/ticker/24hr?symbol=<symbol>. On MARKET_DATA_OK,
    // *out_ticker is fully populated. MARKET_DATA_ERR_SYMBOL_NOT_FOUND is
    // returned for a symbol Binance doesn't recognize (HTTP 400 + Binance error
    // code -1121 "Invalid symbol") - same convention as
    // market_data_client_fetch_symbol_status(). Unlike exchangeInfo, this
    // endpoint has no separate "exists but not tradable" state to report, so a
    // successful fetch is sufficient evidence the pair is valid and quotable.
    market_data_err_t market_data_client_fetch_ticker_24hr(const char *symbol, market_data_ticker_24hr_t *out_ticker);

    // --- klines ---

#define MARKET_DATA_KLINES_MAX_LIMIT                                                                                   \
    500                                 // Binance's documented per-request ceiling; this
                                        // client does not paginate beyond it
#define MARKET_DATA_KLINES_V1_LIMIT 288 // 24h of 5m candles: 24*60/5
#define MARKET_DATA_KLINE_INTERVAL_5M "5m"

    typedef struct
    {
        const char *symbol;    // required, e.g. "BTCUSDT"
        const char *interval;  // required, Binance interval string, e.g. "5m", "1h"
        uint16_t limit;        // 0 => server default; else 1..MARKET_DATA_KLINES_MAX_LIMIT
        int64_t start_time_ms; // 0 => omit startTime query param
        int64_t end_time_ms;   // 0 => omit endTime query param
    } market_data_klines_request_t;

    typedef struct
    {
        int64_t open_time_ms;
        double open;
        double high;
        double low;
        double close;
        double volume;
        int64_t close_time_ms;
        double quote_asset_volume;
        uint32_t number_of_trades;
        double taker_buy_base_volume;
        double taker_buy_quote_volume;
        // "ignore" field intentionally dropped - unused, per Binance docs.
    } market_data_kline_t;

    // Caller owns out_klines (storage for at least out_capacity entries) - use a
    // static/heap buffer, not a task's default stack (sizeof(market_data_kline_t)
    // * MARKET_DATA_KLINES_V1_LIMIT is ~25KB). Rows are streamed directly into it
    // as they're parsed, oldest first (Binance's native order), never past
    // out_capacity. If Binance's response contains more rows than out_capacity,
    // returns MARKET_DATA_ERR_INVALID_ARG - rows are never silently truncated.
    market_data_err_t market_data_client_fetch_klines(const market_data_klines_request_t *req,
                                                      market_data_kline_t *out_klines, uint16_t out_capacity,
                                                      uint16_t *out_count);

    // v1 convenience wrapper: symbol, interval=5m, limit=288, no start/end (most
    // recent 24h). out_capacity must be >= MARKET_DATA_KLINES_V1_LIMIT.
    market_data_err_t market_data_client_fetch_klines_24h_5m(const char *symbol, market_data_kline_t *out_klines,
                                                             uint16_t out_capacity, uint16_t *out_count);

    // --- batch klines (watchlist) ---

    typedef struct
    {
        const char *symbol; // borrowed from the symbols[] array passed in - must outlive the call
        market_data_err_t err;
        uint16_t count; // rows written to the matching out_klines_per_symbol[] entry; 0 if err != MARKET_DATA_OK
    } market_data_batch_result_t;

    // Fetches 24h/5m klines (same shape as market_data_client_fetch_klines_24h_5m)
    // for symbol_count symbols, one after another on a single reused HTTP
    // session (see market_data_http_next()) instead of opening/closing a new
    // client per symbol. A failure on one symbol is recorded in
    // out_results[i] and does not stop the rest of the batch - callers get a
    // best-effort result per symbol, checked via out_results[i].err.
    //
    // out_klines_per_symbol[i] must point to a buffer of at least
    // out_capacity_per_symbol entries; out_results must have symbol_count
    // entries. Returns MARKET_DATA_ERR_INVALID_ARG for bad arguments (checked
    // up front, before anything is fetched) and MARKET_DATA_ERR_NOT_SYNCED if
    // the clock isn't synced yet - in both cases no requests are made. Once the
    // batch actually starts, this always returns MARKET_DATA_OK; per-symbol
    // outcomes are in out_results.
    market_data_err_t market_data_client_fetch_klines_24h_5m_batch(const char *const *symbols, uint8_t symbol_count,
                                                                   market_data_kline_t *const *out_klines_per_symbol,
                                                                   uint16_t out_capacity_per_symbol,
                                                                   market_data_batch_result_t *out_results);

#ifdef __cplusplus
}
#endif
