#pragma once

// Pure C, host-compilable: a minimal incremental (streaming) JSON lexer.
//
// Unlike a DOM parser (e.g. cJSON), this never holds the whole document in
// memory or builds a parse tree - it is fed raw bytes as they arrive off the
// socket (see market_data_http.c) and produces one token at a time. Lexer
// state that would otherwise be lost between chunks (e.g. a decimal string
// cut mid-digit by a TCP read boundary) is kept in market_data_json_scanner_t
// and resumed on the next call. This is what lets the klines/symbol parsers
// built on top of it avoid ever buffering a full ~40KB response body or
// building a ~200KB DOM tree for it.
//
// Grammar (structural) state lives one layer up, in market_data_klines_parser
// / market_data_symbol_parser - this module only knows about JSON tokens, not
// what they mean.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Large enough for any string/number field this project's two grammars
// actually need to hold (Binance decimal strings, status/permission
// strings, exchangeInfo filter values) - all well under 32 bytes in
// practice. A field that doesn't fit is treated as a parse error rather
// than silently truncated.
#define MARKET_DATA_JSON_SCRATCH_MAX 96

    typedef enum
    {
        MARKET_DATA_JSON_TOK_NONE = 0, // no complete token yet; feed more bytes
        MARKET_DATA_JSON_TOK_ARRAY_START,
        MARKET_DATA_JSON_TOK_ARRAY_END,
        MARKET_DATA_JSON_TOK_OBJECT_START,
        MARKET_DATA_JSON_TOK_OBJECT_END,
        MARKET_DATA_JSON_TOK_COMMA,
        MARKET_DATA_JSON_TOK_COLON,
        MARKET_DATA_JSON_TOK_STRING,
        MARKET_DATA_JSON_TOK_NUMBER,
        MARKET_DATA_JSON_TOK_TRUE,
        MARKET_DATA_JSON_TOK_FALSE,
        MARKET_DATA_JSON_TOK_NULL,
        MARKET_DATA_JSON_TOK_ERROR, // malformed input; scanner must not be reused
    } market_data_json_token_kind_t;

    typedef enum
    {
        MARKET_DATA_JSON_LEX_BETWEEN = 0,
        MARKET_DATA_JSON_LEX_IN_STRING,
        MARKET_DATA_JSON_LEX_IN_STRING_ESCAPE,
        MARKET_DATA_JSON_LEX_IN_NUMBER,
        MARKET_DATA_JSON_LEX_IN_LITERAL,
    } market_data_json_lex_state_t;

    typedef struct
    {
        market_data_json_lex_state_t lex_state;
        char scratch[MARKET_DATA_JSON_SCRATCH_MAX];
        size_t scratch_len;
    } market_data_json_scanner_t;

    void market_data_json_scanner_init(market_data_json_scanner_t *sc);

    // Consumes bytes from *pbuf (of *plen bytes), advancing both past whatever
    // was consumed, until either one complete token is produced or the input is
    // exhausted. On MARKET_DATA_JSON_TOK_NONE, *plen is always 0 on return - all
    // available bytes were consumed into internal state and the caller should
    // feed() more data before calling again. On MARKET_DATA_JSON_TOK_STRING, the
    // decoded text is available via market_data_json_scanner_text() until the
    // next call to this function. On MARKET_DATA_JSON_TOK_NUMBER, *out_number
    // holds the parsed value.
    market_data_json_token_kind_t market_data_json_scanner_next(market_data_json_scanner_t *sc, const char **pbuf,
                                                                size_t *plen, double *out_number);

    // Valid only for the token just returned by market_data_json_scanner_next()
    // (MARKET_DATA_JSON_TOK_STRING), and only until the next call to that
    // function - the caller must copy/compare before requesting the next token.
    const char *market_data_json_scanner_text(const market_data_json_scanner_t *sc);
    size_t market_data_json_scanner_text_len(const market_data_json_scanner_t *sc);

#ifdef __cplusplus
}
#endif
