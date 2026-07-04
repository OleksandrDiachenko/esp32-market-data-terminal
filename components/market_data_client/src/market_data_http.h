#pragma once

// ESP-IDF glue: blocking HTTPS GET on top of esp_http_client, split into
// open (headers+status) / stream-body / read-snippet / close so the caller
// (market_data_client.c) can decide how to consume the body only after
// seeing the status code, without ever buffering the whole response itself
// - stream_body() feeds fixed-size chunks straight into a caller-supplied
// sink (a streaming JSON parser's feed() function in practice). Not
// host-testable (depends on esp_http_client); the streaming parsers it
// feeds are tested independently in host_test/.

#include <stddef.h>
#include <stdint.h>

#include "market_data_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct market_data_http_session market_data_http_session_t;

// Returns non-OK to abort market_data_http_stream_body() early (e.g. a
// streaming parser detected malformed JSON or a capacity overflow).
typedef market_data_err_t (*market_data_http_body_sink_t)(void *ctx, const char *chunk, size_t len);

// Opens a blocking HTTPS GET and fetches response headers/status, but does
// not read the body. On MARKET_DATA_OK, *out_session must later be released
// via market_data_http_close() and *out_status holds the HTTP status code.
market_data_err_t market_data_http_open(const char *url, uint32_t timeout_ms, market_data_http_session_t **out_session,
                                         int *out_status);

// Ends the current request on an already-open session (closes its
// transport connection) and issues a new GET for url on the *same*
// underlying esp_http_client handle - skips the esp_http_client_init()/
// esp_http_client_cleanup() allocation churn a fresh market_data_http_open()
// would pay, and lets the handle resume a prior TLS session (when
// CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS is enabled) instead of a full
// handshake. Every request still opens its own TCP+TLS connection -
// esp_http_client always reconnects on open() - the savings are the
// init/cleanup overhead and the possible TLS session resumption, not a
// literal single kept-open socket across requests.
//
// Only call this after the current request's body has been fully consumed
// (via market_data_http_stream_body() or _read_body_snippet()). On any
// return value the session handle itself remains valid and must still be
// released via market_data_http_close() once the whole request sequence is
// done - a failure here does not tear down the session.
market_data_err_t market_data_http_next(market_data_http_session_t *session, const char *url, uint32_t timeout_ms,
                                         int *out_status);

// Reads the response body in small fixed-size chunks, feeding each to
// sink(ctx, chunk, len) as it arrives - never buffers the full body.
market_data_err_t market_data_http_stream_body(market_data_http_session_t *session, market_data_http_body_sink_t sink,
                                                void *sink_ctx);

// Reads up to out_capacity-1 bytes of the body into out (NUL-terminated) -
// for small diagnostic/error bodies, not the success-path streaming parse.
market_data_err_t market_data_http_read_body_snippet(market_data_http_session_t *session, char *out,
                                                       size_t out_capacity, size_t *out_len);

void market_data_http_close(market_data_http_session_t *session);

#ifdef __cplusplus
}
#endif
