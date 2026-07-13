#include "market_data_ws_stream_parser.h"

#include <stdlib.h>
#include <string.h>

typedef enum
{
    MARKET_DATA_WSP_LEVEL_ROOT,
    MARKET_DATA_WSP_LEVEL_DATA,
    MARKET_DATA_WSP_LEVEL_K,
} market_data_wsp_level_t;

static market_data_ws_stream_parser_key_t key_from_text(const char *text, market_data_wsp_level_t level)
{
    if (level == MARKET_DATA_WSP_LEVEL_ROOT)
    {
        return strcmp(text, "data") == 0 ? MARKET_DATA_WSP_KEY_DATA : MARKET_DATA_WSP_KEY_OTHER;
    }
    if (level == MARKET_DATA_WSP_LEVEL_DATA)
    {
        if (strcmp(text, "e") == 0)
        {
            return MARKET_DATA_WSP_KEY_EVENT_TYPE;
        }
        if (strcmp(text, "s") == 0)
        {
            return MARKET_DATA_WSP_KEY_SYMBOL;
        }
        if (strcmp(text, "k") == 0)
        {
            return MARKET_DATA_WSP_KEY_K;
        }
        return MARKET_DATA_WSP_KEY_OTHER;
    }
    // MARKET_DATA_WSP_LEVEL_K
    if (strcmp(text, "t") == 0)
    {
        return MARKET_DATA_WSP_KEY_OPEN_TIME;
    }
    if (strcmp(text, "o") == 0)
    {
        return MARKET_DATA_WSP_KEY_OPEN;
    }
    if (strcmp(text, "h") == 0)
    {
        return MARKET_DATA_WSP_KEY_HIGH;
    }
    if (strcmp(text, "l") == 0)
    {
        return MARKET_DATA_WSP_KEY_LOW;
    }
    if (strcmp(text, "c") == 0)
    {
        return MARKET_DATA_WSP_KEY_CLOSE;
    }
    if (strcmp(text, "v") == 0)
    {
        return MARKET_DATA_WSP_KEY_VOLUME;
    }
    if (strcmp(text, "n") == 0)
    {
        return MARKET_DATA_WSP_KEY_NUM_TRADES;
    }
    if (strcmp(text, "x") == 0)
    {
        return MARKET_DATA_WSP_KEY_IS_FINAL;
    }
    return MARKET_DATA_WSP_KEY_OTHER;
}

// Advances a generic "skip this value" depth counter given the next token -
// identical in spirit to market_data_symbol_parser.c's skip_advance().
static bool skip_advance(int *depth, market_data_json_token_kind_t tok)
{
    bool is_open = (tok == MARKET_DATA_JSON_TOK_ARRAY_START || tok == MARKET_DATA_JSON_TOK_OBJECT_START);
    bool is_close = (tok == MARKET_DATA_JSON_TOK_ARRAY_END || tok == MARKET_DATA_JSON_TOK_OBJECT_END);

    if (*depth == 0)
    {
        if (is_open)
        {
            *depth = 1;
            return false;
        }
        return true; // scalar value, nothing to skip
    }
    if (is_open)
    {
        (*depth)++;
    }
    else if (is_close)
    {
        (*depth)--;
    }
    return *depth == 0;
}

void market_data_ws_stream_parser_init(market_data_ws_stream_parser_t *p, market_data_kline_update_t *out_update)
{
    memset(p, 0, sizeof(*p));
    market_data_json_scanner_init(&p->scanner);
    p->state = MARKET_DATA_WSP_EXPECT_ROOT_OBJ_START;
    p->out_update = out_update;
    memset(out_update, 0, sizeof(*out_update));
}

static bool k_fields_complete(const market_data_ws_stream_parser_t *p)
{
    return p->saw_t && p->saw_o && p->saw_h && p->saw_l && p->saw_c && p->saw_v && p->saw_n && p->saw_x;
}

market_data_err_t market_data_ws_stream_parser_feed(market_data_ws_stream_parser_t *p, const char *buf, size_t len)
{
    while (len > 0 && p->state != MARKET_DATA_WSP_DONE && p->state != MARKET_DATA_WSP_ERROR)
    {
        double num = 0;
        market_data_json_token_kind_t tok = market_data_json_scanner_next(&p->scanner, &buf, &len, &num);
        if (tok == MARKET_DATA_JSON_TOK_NONE)
        {
            break;
        }
        if (tok == MARKET_DATA_JSON_TOK_ERROR)
        {
            goto fail;
        }

        switch (p->state)
        {
        case MARKET_DATA_WSP_EXPECT_ROOT_OBJ_START:
            if (tok != MARKET_DATA_JSON_TOK_OBJECT_START)
            {
                goto fail;
            }
            p->state = MARKET_DATA_WSP_EXPECT_ROOT_KEY_OR_END;
            break;

        case MARKET_DATA_WSP_EXPECT_ROOT_KEY_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                p->state = MARKET_DATA_WSP_DONE;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_STRING)
            {
                goto fail;
            }
            p->pending_key = key_from_text(market_data_json_scanner_text(&p->scanner), MARKET_DATA_WSP_LEVEL_ROOT);
            p->state = MARKET_DATA_WSP_EXPECT_ROOT_COLON;
            break;

        case MARKET_DATA_WSP_EXPECT_ROOT_COLON:
            if (tok != MARKET_DATA_JSON_TOK_COLON)
            {
                goto fail;
            }
            p->state = MARKET_DATA_WSP_EXPECT_ROOT_VALUE;
            break;

        case MARKET_DATA_WSP_EXPECT_ROOT_VALUE:
            if (p->pending_key == MARKET_DATA_WSP_KEY_DATA)
            {
                if (tok != MARKET_DATA_JSON_TOK_OBJECT_START)
                {
                    goto fail;
                }
                p->state = MARKET_DATA_WSP_EXPECT_DATA_KEY_OR_END;
            }
            else
            {
                p->skip_depth = 0;
                p->state = skip_advance(&p->skip_depth, tok) ? MARKET_DATA_WSP_EXPECT_ROOT_COMMA_OR_END
                                                             : MARKET_DATA_WSP_SKIP_ROOT_VALUE;
            }
            break;

        case MARKET_DATA_WSP_EXPECT_ROOT_COMMA_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_WSP_EXPECT_ROOT_KEY_OR_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                p->state = MARKET_DATA_WSP_DONE;
            }
            else
            {
                goto fail;
            }
            break;

        case MARKET_DATA_WSP_SKIP_ROOT_VALUE:
            if (skip_advance(&p->skip_depth, tok))
            {
                p->state = MARKET_DATA_WSP_EXPECT_ROOT_COMMA_OR_END;
            }
            break;

        case MARKET_DATA_WSP_EXPECT_DATA_KEY_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (p->saw_e_kline && (!p->saw_k || !p->saw_symbol))
                {
                    goto fail; // claimed to be a kline event but incomplete
                }
                p->state = MARKET_DATA_WSP_EXPECT_ROOT_COMMA_OR_END;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_STRING)
            {
                goto fail;
            }
            p->pending_key = key_from_text(market_data_json_scanner_text(&p->scanner), MARKET_DATA_WSP_LEVEL_DATA);
            p->state = MARKET_DATA_WSP_EXPECT_DATA_COLON;
            break;

        case MARKET_DATA_WSP_EXPECT_DATA_COLON:
            if (tok != MARKET_DATA_JSON_TOK_COLON)
            {
                goto fail;
            }
            p->state = MARKET_DATA_WSP_EXPECT_DATA_VALUE;
            break;

        case MARKET_DATA_WSP_EXPECT_DATA_VALUE:
            if (p->pending_key == MARKET_DATA_WSP_KEY_EVENT_TYPE)
            {
                if (tok != MARKET_DATA_JSON_TOK_STRING)
                {
                    goto fail;
                }
                p->saw_e_kline = strcmp(market_data_json_scanner_text(&p->scanner), "kline") == 0;
                p->state = MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END;
            }
            else if (p->pending_key == MARKET_DATA_WSP_KEY_SYMBOL)
            {
                if (tok != MARKET_DATA_JSON_TOK_STRING)
                {
                    goto fail;
                }
                const char *text = market_data_json_scanner_text(&p->scanner);
                if (market_data_json_scanner_text_len(&p->scanner) > SETTINGS_SYMBOL_MAX_LEN)
                {
                    goto fail; // too long to fit - reject rather than truncate
                }
                strncpy(p->out_update->symbol, text, SETTINGS_SYMBOL_MAX_LEN);
                p->out_update->symbol[SETTINGS_SYMBOL_MAX_LEN] = '\0';
                p->saw_symbol = true;
                p->state = MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END;
            }
            else if (p->pending_key == MARKET_DATA_WSP_KEY_K)
            {
                if (tok != MARKET_DATA_JSON_TOK_OBJECT_START)
                {
                    goto fail;
                }
                p->state = MARKET_DATA_WSP_EXPECT_K_KEY_OR_END;
            }
            else
            {
                p->skip_depth = 0;
                p->state = skip_advance(&p->skip_depth, tok) ? MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END
                                                             : MARKET_DATA_WSP_SKIP_DATA_VALUE;
            }
            break;

        case MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_WSP_EXPECT_DATA_KEY_OR_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (p->saw_e_kline && (!p->saw_k || !p->saw_symbol))
                {
                    goto fail;
                }
                p->state = MARKET_DATA_WSP_EXPECT_ROOT_COMMA_OR_END;
            }
            else
            {
                goto fail;
            }
            break;

        case MARKET_DATA_WSP_SKIP_DATA_VALUE:
            if (skip_advance(&p->skip_depth, tok))
            {
                p->state = MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END;
            }
            break;

        case MARKET_DATA_WSP_EXPECT_K_KEY_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (!k_fields_complete(p))
                {
                    goto fail;
                }
                p->saw_k = true;
                p->state = MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_STRING)
            {
                goto fail;
            }
            p->pending_key = key_from_text(market_data_json_scanner_text(&p->scanner), MARKET_DATA_WSP_LEVEL_K);
            p->state = MARKET_DATA_WSP_EXPECT_K_COLON;
            break;

        case MARKET_DATA_WSP_EXPECT_K_COLON:
            if (tok != MARKET_DATA_JSON_TOK_COLON)
            {
                goto fail;
            }
            p->state = MARKET_DATA_WSP_EXPECT_K_VALUE;
            break;

        case MARKET_DATA_WSP_EXPECT_K_VALUE:
            switch (p->pending_key)
            {
            case MARKET_DATA_WSP_KEY_OPEN_TIME:
                if (tok != MARKET_DATA_JSON_TOK_NUMBER)
                {
                    goto fail;
                }
                // 13-digit millisecond timestamps are well under 2^53, so
                // this cast from the scanner's double representation is
                // exact (same reasoning as market_data_klines_parser.c).
                p->out_update->open_time_ms = (int64_t)num;
                p->saw_t = true;
                p->state = MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END;
                break;

            case MARKET_DATA_WSP_KEY_NUM_TRADES:
                if (tok != MARKET_DATA_JSON_TOK_NUMBER)
                {
                    goto fail;
                }
                p->out_update->number_of_trades = (uint32_t)num;
                p->saw_n = true;
                p->state = MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END;
                break;

            case MARKET_DATA_WSP_KEY_IS_FINAL:
                if (tok == MARKET_DATA_JSON_TOK_TRUE)
                {
                    p->out_update->is_final = true;
                }
                else if (tok == MARKET_DATA_JSON_TOK_FALSE)
                {
                    p->out_update->is_final = false;
                }
                else
                {
                    goto fail;
                }
                p->saw_x = true;
                p->state = MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END;
                break;

            case MARKET_DATA_WSP_KEY_OPEN:
            case MARKET_DATA_WSP_KEY_HIGH:
            case MARKET_DATA_WSP_KEY_LOW:
            case MARKET_DATA_WSP_KEY_CLOSE:
            case MARKET_DATA_WSP_KEY_VOLUME:
            {
                // Binance sends these as decimal strings, never JSON numbers
                // - reject the wrong type rather than silently coercing it
                // (same philosophy as market_data_klines_parser.c).
                if (tok != MARKET_DATA_JSON_TOK_STRING)
                {
                    goto fail;
                }
                const char *text = market_data_json_scanner_text(&p->scanner);
                char *endptr = NULL;
                double value = strtod(text, &endptr);
                if (endptr == text)
                {
                    goto fail;
                }
                switch (p->pending_key)
                {
                case MARKET_DATA_WSP_KEY_OPEN:
                    p->out_update->open = value;
                    p->saw_o = true;
                    break;
                case MARKET_DATA_WSP_KEY_HIGH:
                    p->out_update->high = value;
                    p->saw_h = true;
                    break;
                case MARKET_DATA_WSP_KEY_LOW:
                    p->out_update->low = value;
                    p->saw_l = true;
                    break;
                case MARKET_DATA_WSP_KEY_CLOSE:
                    p->out_update->close = value;
                    p->saw_c = true;
                    break;
                case MARKET_DATA_WSP_KEY_VOLUME:
                    p->out_update->volume = value;
                    p->saw_v = true;
                    break;
                default:
                    break;
                }
                p->state = MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END;
                break;
            }

            default:
                p->skip_depth = 0;
                p->state = skip_advance(&p->skip_depth, tok) ? MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END
                                                             : MARKET_DATA_WSP_SKIP_K_VALUE;
                break;
            }
            break;

        case MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_WSP_EXPECT_K_KEY_OR_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (!k_fields_complete(p))
                {
                    goto fail;
                }
                p->saw_k = true;
                p->state = MARKET_DATA_WSP_EXPECT_DATA_COMMA_OR_END;
            }
            else
            {
                goto fail;
            }
            break;

        case MARKET_DATA_WSP_SKIP_K_VALUE:
            if (skip_advance(&p->skip_depth, tok))
            {
                p->state = MARKET_DATA_WSP_EXPECT_K_COMMA_OR_END;
            }
            break;

        case MARKET_DATA_WSP_DONE:
        case MARKET_DATA_WSP_ERROR:
            break;
        }
    }
    return MARKET_DATA_OK;

fail:
    p->state = MARKET_DATA_WSP_ERROR;
    return MARKET_DATA_ERR_PARSE;
}

market_data_ws_parse_result_t market_data_ws_stream_parser_finish(const market_data_ws_stream_parser_t *p)
{
    if (p->state != MARKET_DATA_WSP_DONE)
    {
        return MARKET_DATA_WS_PARSE_ERROR;
    }
    if (!p->saw_e_kline)
    {
        return MARKET_DATA_WS_PARSE_IGNORED;
    }
    // saw_e_kline implies saw_k && saw_symbol already (enforced when the
    // data/k objects closed above), and saw_k implies k_fields_complete().
    return MARKET_DATA_WS_PARSE_UPDATE;
}
