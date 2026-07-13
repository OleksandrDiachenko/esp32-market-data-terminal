#include "market_data_ticker_parser.h"

#include <stdlib.h>
#include <string.h>

static market_data_ticker_parser_key_t key_from_text(const char *text)
{
    if (strcmp(text, "lastPrice") == 0)
    {
        return MARKET_DATA_TIP_KEY_LAST_PRICE;
    }
    if (strcmp(text, "priceChangePercent") == 0)
    {
        return MARKET_DATA_TIP_KEY_PRICE_CHANGE_PERCENT;
    }
    if (strcmp(text, "highPrice") == 0)
    {
        return MARKET_DATA_TIP_KEY_HIGH_PRICE;
    }
    if (strcmp(text, "lowPrice") == 0)
    {
        return MARKET_DATA_TIP_KEY_LOW_PRICE;
    }
    return MARKET_DATA_TIP_KEY_OTHER;
}

// Same "generic skip" helper as market_data_symbol_parser - advances a depth
// counter given the next token, returning true once a scalar/array/object
// value has been fully consumed.
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
        return true;
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

void market_data_ticker_parser_init(market_data_ticker_parser_t *p, market_data_ticker_24hr_t *out_ticker)
{
    memset(p, 0, sizeof(*p));
    market_data_json_scanner_init(&p->scanner);
    p->state = MARKET_DATA_TIP_EXPECT_OBJ_START;
    p->out_ticker = out_ticker;
    out_ticker->last_price = 0;
    out_ticker->price_change_percent = 0;
    out_ticker->high_price = 0;
    out_ticker->low_price = 0;
}

static market_data_err_t capture_decimal_field(market_data_ticker_parser_t *p, market_data_json_token_kind_t tok)
{
    if (tok != MARKET_DATA_JSON_TOK_STRING)
    {
        return MARKET_DATA_ERR_PARSE;
    }
    const char *text = market_data_json_scanner_text(&p->scanner);
    char *endptr = NULL;
    double value = strtod(text, &endptr);
    if (endptr == text)
    {
        return MARKET_DATA_ERR_PARSE;
    }
    switch (p->pending_key)
    {
    case MARKET_DATA_TIP_KEY_LAST_PRICE:
        p->out_ticker->last_price = value;
        p->saw_last_price = true;
        break;
    case MARKET_DATA_TIP_KEY_PRICE_CHANGE_PERCENT:
        p->out_ticker->price_change_percent = value;
        p->saw_price_change_percent = true;
        break;
    case MARKET_DATA_TIP_KEY_HIGH_PRICE:
        p->out_ticker->high_price = value;
        p->saw_high_price = true;
        break;
    case MARKET_DATA_TIP_KEY_LOW_PRICE:
        p->out_ticker->low_price = value;
        p->saw_low_price = true;
        break;
    default:
        break;
    }
    return MARKET_DATA_OK;
}

static bool all_fields_seen(const market_data_ticker_parser_t *p)
{
    return p->saw_last_price && p->saw_price_change_percent && p->saw_high_price && p->saw_low_price;
}

market_data_err_t market_data_ticker_parser_feed(market_data_ticker_parser_t *p, const char *buf, size_t len)
{
    while (len > 0 && p->state != MARKET_DATA_TIP_DONE && p->state != MARKET_DATA_TIP_ERROR)
    {
        double num = 0;
        market_data_json_token_kind_t tok = market_data_json_scanner_next(&p->scanner, &buf, &len, &num);
        (void)num;
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
        case MARKET_DATA_TIP_EXPECT_OBJ_START:
            if (tok != MARKET_DATA_JSON_TOK_OBJECT_START)
            {
                goto fail;
            }
            p->state = MARKET_DATA_TIP_EXPECT_KEY_OR_END;
            break;

        case MARKET_DATA_TIP_EXPECT_KEY_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (!all_fields_seen(p))
                {
                    goto fail;
                }
                p->state = MARKET_DATA_TIP_DONE;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_STRING)
            {
                goto fail;
            }
            p->pending_key = key_from_text(market_data_json_scanner_text(&p->scanner));
            p->state = MARKET_DATA_TIP_EXPECT_COLON;
            break;

        case MARKET_DATA_TIP_EXPECT_COLON:
            if (tok != MARKET_DATA_JSON_TOK_COLON)
            {
                goto fail;
            }
            p->state = MARKET_DATA_TIP_EXPECT_VALUE;
            break;

        case MARKET_DATA_TIP_EXPECT_VALUE:
            if (p->pending_key != MARKET_DATA_TIP_KEY_OTHER)
            {
                market_data_err_t err = capture_decimal_field(p, tok);
                if (err != MARKET_DATA_OK)
                {
                    goto fail;
                }
                p->state = MARKET_DATA_TIP_EXPECT_COMMA_OR_END;
            }
            else
            {
                p->skip_depth = 0;
                p->state = skip_advance(&p->skip_depth, tok) ? MARKET_DATA_TIP_EXPECT_COMMA_OR_END
                                                             : MARKET_DATA_TIP_SKIP_VALUE;
            }
            break;

        case MARKET_DATA_TIP_EXPECT_COMMA_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_TIP_EXPECT_KEY_OR_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (!all_fields_seen(p))
                {
                    goto fail;
                }
                p->state = MARKET_DATA_TIP_DONE;
            }
            else
            {
                goto fail;
            }
            break;

        case MARKET_DATA_TIP_SKIP_VALUE:
            if (skip_advance(&p->skip_depth, tok))
            {
                p->state = MARKET_DATA_TIP_EXPECT_COMMA_OR_END;
            }
            break;

        case MARKET_DATA_TIP_DONE:
        case MARKET_DATA_TIP_ERROR:
            break;
        }
    }
    return MARKET_DATA_OK;

fail:
    p->state = MARKET_DATA_TIP_ERROR;
    return MARKET_DATA_ERR_PARSE;
}

market_data_err_t market_data_ticker_parser_finish(const market_data_ticker_parser_t *p)
{
    return p->state == MARKET_DATA_TIP_DONE ? MARKET_DATA_OK : MARKET_DATA_ERR_PARSE;
}
