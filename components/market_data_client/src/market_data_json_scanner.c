#include "market_data_json_scanner.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void market_data_json_scanner_init(market_data_json_scanner_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->lex_state = MARKET_DATA_JSON_LEX_BETWEEN;
}

const char *market_data_json_scanner_text(const market_data_json_scanner_t *sc) { return sc->scratch; }

size_t market_data_json_scanner_text_len(const market_data_json_scanner_t *sc) { return sc->scratch_len; }

static bool is_json_whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static bool is_number_char(char c)
{
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static bool scratch_append(market_data_json_scanner_t *sc, char c)
{
    // Room for the terminating NUL is reserved up front.
    if (sc->scratch_len + 1 >= MARKET_DATA_JSON_SCRATCH_MAX)
    {
        return false;
    }
    sc->scratch[sc->scratch_len++] = c;
    return true;
}

// Single-char structural tokens consume the byte and return immediately.
static market_data_json_token_kind_t single_char_token(char c)
{
    switch (c)
    {
    case '[':
        return MARKET_DATA_JSON_TOK_ARRAY_START;
    case ']':
        return MARKET_DATA_JSON_TOK_ARRAY_END;
    case '{':
        return MARKET_DATA_JSON_TOK_OBJECT_START;
    case '}':
        return MARKET_DATA_JSON_TOK_OBJECT_END;
    case ',':
        return MARKET_DATA_JSON_TOK_COMMA;
    case ':':
        return MARKET_DATA_JSON_TOK_COLON;
    default:
        return MARKET_DATA_JSON_TOK_NONE;
    }
}

market_data_json_token_kind_t market_data_json_scanner_next(market_data_json_scanner_t *sc, const char **pbuf,
                                                            size_t *plen, double *out_number)
{
    const char *buf = *pbuf;
    size_t len = *plen;
    size_t i = 0;

    for (;;)
    {
        if (i >= len)
        {
            *pbuf = buf + i;
            *plen = 0;
            return MARKET_DATA_JSON_TOK_NONE;
        }

        char c = buf[i];

        switch (sc->lex_state)
        {
        case MARKET_DATA_JSON_LEX_BETWEEN:
        {
            if (is_json_whitespace(c))
            {
                i++;
                continue;
            }
            market_data_json_token_kind_t single = single_char_token(c);
            if (single != MARKET_DATA_JSON_TOK_NONE)
            {
                i++;
                *pbuf = buf + i;
                *plen = len - i;
                return single;
            }
            if (c == '"')
            {
                i++;
                sc->lex_state = MARKET_DATA_JSON_LEX_IN_STRING;
                sc->scratch_len = 0;
                continue;
            }
            if (c == '-' || (c >= '0' && c <= '9'))
            {
                sc->lex_state = MARKET_DATA_JSON_LEX_IN_NUMBER;
                sc->scratch_len = 0;
                continue; // reprocess c under IN_NUMBER, don't consume yet
            }
            if (isalpha((unsigned char)c))
            {
                sc->lex_state = MARKET_DATA_JSON_LEX_IN_LITERAL;
                sc->scratch_len = 0;
                continue; // reprocess c under IN_LITERAL
            }
            *pbuf = buf + i;
            *plen = len - i;
            return MARKET_DATA_JSON_TOK_ERROR;
        }

        case MARKET_DATA_JSON_LEX_IN_STRING:
        {
            if (c == '\\')
            {
                i++;
                sc->lex_state = MARKET_DATA_JSON_LEX_IN_STRING_ESCAPE;
                continue;
            }
            if (c == '"')
            {
                i++;
                sc->lex_state = MARKET_DATA_JSON_LEX_BETWEEN;
                sc->scratch[sc->scratch_len] = '\0';
                *pbuf = buf + i;
                *plen = len - i;
                return MARKET_DATA_JSON_TOK_STRING;
            }
            if (!scratch_append(sc, c))
            {
                *pbuf = buf + i;
                *plen = len - i;
                return MARKET_DATA_JSON_TOK_ERROR;
            }
            i++;
            continue;
        }

        case MARKET_DATA_JSON_LEX_IN_STRING_ESCAPE:
        {
            char decoded;
            switch (c)
            {
            case '"':
                decoded = '"';
                break;
            case '\\':
                decoded = '\\';
                break;
            case '/':
                decoded = '/';
                break;
            case 'b':
                decoded = '\b';
                break;
            case 'f':
                decoded = '\f';
                break;
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            default:
                // Includes \uXXXX: none of the fields these grammars care
                // about are ever non-ASCII, so treat it as unsupported
                // rather than silently mis-decoding.
                *pbuf = buf + i;
                *plen = len - i;
                return MARKET_DATA_JSON_TOK_ERROR;
            }
            if (!scratch_append(sc, decoded))
            {
                *pbuf = buf + i;
                *plen = len - i;
                return MARKET_DATA_JSON_TOK_ERROR;
            }
            i++;
            sc->lex_state = MARKET_DATA_JSON_LEX_IN_STRING;
            continue;
        }

        case MARKET_DATA_JSON_LEX_IN_NUMBER:
        {
            if (is_number_char(c))
            {
                if (!scratch_append(sc, c))
                {
                    *pbuf = buf + i;
                    *plen = len - i;
                    return MARKET_DATA_JSON_TOK_ERROR;
                }
                i++;
                continue;
            }
            // Delimiter: do not consume it, the caller (BETWEEN state) will.
            sc->lex_state = MARKET_DATA_JSON_LEX_BETWEEN;
            sc->scratch[sc->scratch_len] = '\0';
            char *endptr = NULL;
            double value = strtod(sc->scratch, &endptr);
            *pbuf = buf + i;
            *plen = len - i;
            if (endptr == sc->scratch || sc->scratch_len == 0)
            {
                return MARKET_DATA_JSON_TOK_ERROR;
            }
            if (out_number != NULL)
            {
                *out_number = value;
            }
            return MARKET_DATA_JSON_TOK_NUMBER;
        }

        case MARKET_DATA_JSON_LEX_IN_LITERAL:
        {
            if (isalpha((unsigned char)c))
            {
                if (!scratch_append(sc, c))
                {
                    *pbuf = buf + i;
                    *plen = len - i;
                    return MARKET_DATA_JSON_TOK_ERROR;
                }
                i++;
                continue;
            }
            sc->lex_state = MARKET_DATA_JSON_LEX_BETWEEN;
            sc->scratch[sc->scratch_len] = '\0';
            *pbuf = buf + i;
            *plen = len - i;
            if (strcmp(sc->scratch, "true") == 0)
            {
                return MARKET_DATA_JSON_TOK_TRUE;
            }
            if (strcmp(sc->scratch, "false") == 0)
            {
                return MARKET_DATA_JSON_TOK_FALSE;
            }
            if (strcmp(sc->scratch, "null") == 0)
            {
                return MARKET_DATA_JSON_TOK_NULL;
            }
            return MARKET_DATA_JSON_TOK_ERROR;
        }
        }
    }
}
