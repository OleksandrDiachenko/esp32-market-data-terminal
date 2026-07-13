#include "market_data_klines_parser.h"

#include <stdlib.h>
#include <string.h>

void market_data_klines_parser_init(market_data_klines_parser_t *p, market_data_kline_t *out_klines,
                                    uint16_t out_capacity)
{
    memset(p, 0, sizeof(*p));
    market_data_json_scanner_init(&p->scanner);
    p->state = MARKET_DATA_KLP_EXPECT_OUTER_START;
    p->out_klines = out_klines;
    p->out_capacity = out_capacity;
    p->out_count = 0;
}

// Binance kline row layout (12 fields):
//   0 openTime (number,ms)  1 open (string)      2 high (string)
//   3 low (string)          4 close (string)     5 volume (string)
//   6 closeTime (number,ms) 7 quoteAssetVolume (string)
//   8 numberOfTrades (number)
//   9 takerBuyBaseVolume (string)   10 takerBuyQuoteVolume (string)
//   11 ignore (unused)
static market_data_err_t handle_field_value(market_data_klines_parser_t *p, market_data_json_token_kind_t tok,
                                            double num)
{
    market_data_kline_t *row = &p->out_klines[p->out_count];

    switch (p->field_index)
    {
    case 0:
    case 6:
        if (tok != MARKET_DATA_JSON_TOK_NUMBER)
        {
            return MARKET_DATA_ERR_PARSE;
        }
        // 13-digit millisecond timestamps are well under 2^53, so this cast
        // from the scanner's double representation is exact.
        if (p->field_index == 0)
        {
            row->open_time_ms = (int64_t)num;
        }
        else
        {
            row->close_time_ms = (int64_t)num;
        }
        return MARKET_DATA_OK;

    case 8:
        if (tok != MARKET_DATA_JSON_TOK_NUMBER)
        {
            return MARKET_DATA_ERR_PARSE;
        }
        row->number_of_trades = (uint32_t)num;
        return MARKET_DATA_OK;

    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 7:
    case 9:
    case 10:
    {
        // Binance sends these as decimal strings, never JSON numbers -
        // reject the wrong type rather than silently coercing it.
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
        switch (p->field_index)
        {
        case 1:
            row->open = value;
            break;
        case 2:
            row->high = value;
            break;
        case 3:
            row->low = value;
            break;
        case 4:
            row->close = value;
            break;
        case 5:
            row->volume = value;
            break;
        case 7:
            row->quote_asset_volume = value;
            break;
        case 9:
            row->taker_buy_base_volume = value;
            break;
        case 10:
            row->taker_buy_quote_volume = value;
            break;
        }
        return MARKET_DATA_OK;
    }

    case 11:
        // "ignore" field per Binance docs - accept either JSON type, discard.
        if (tok != MARKET_DATA_JSON_TOK_STRING && tok != MARKET_DATA_JSON_TOK_NUMBER)
        {
            return MARKET_DATA_ERR_PARSE;
        }
        return MARKET_DATA_OK;

    default:
        return MARKET_DATA_ERR_PARSE;
    }
}

market_data_err_t market_data_klines_parser_feed(market_data_klines_parser_t *p, const char *buf, size_t len)
{
    while (len > 0 && p->state != MARKET_DATA_KLP_DONE && p->state != MARKET_DATA_KLP_ERROR)
    {
        double num = 0;
        market_data_json_token_kind_t tok = market_data_json_scanner_next(&p->scanner, &buf, &len, &num);
        if (tok == MARKET_DATA_JSON_TOK_NONE)
        {
            break;
        }
        if (tok == MARKET_DATA_JSON_TOK_ERROR)
        {
            p->state = MARKET_DATA_KLP_ERROR;
            return MARKET_DATA_ERR_PARSE;
        }

        switch (p->state)
        {
        case MARKET_DATA_KLP_EXPECT_OUTER_START:
            if (tok != MARKET_DATA_JSON_TOK_ARRAY_START)
            {
                p->state = MARKET_DATA_KLP_ERROR;
                return MARKET_DATA_ERR_PARSE;
            }
            p->state = MARKET_DATA_KLP_EXPECT_ROW_START_OR_OUTER_END;
            break;

        case MARKET_DATA_KLP_EXPECT_ROW_START_OR_OUTER_END:
            if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                p->state = MARKET_DATA_KLP_DONE;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_ARRAY_START)
            {
                p->state = MARKET_DATA_KLP_ERROR;
                return MARKET_DATA_ERR_PARSE;
            }
            if (p->out_count >= p->out_capacity)
            {
                p->state = MARKET_DATA_KLP_ERROR;
                return MARKET_DATA_ERR_INVALID_ARG;
            }
            p->field_index = 0;
            p->state = MARKET_DATA_KLP_EXPECT_FIELD_VALUE;
            break;

        case MARKET_DATA_KLP_EXPECT_FIELD_VALUE:
        {
            market_data_err_t err = handle_field_value(p, tok, num);
            if (err != MARKET_DATA_OK)
            {
                p->state = MARKET_DATA_KLP_ERROR;
                return err;
            }
            p->state = MARKET_DATA_KLP_EXPECT_FIELD_COMMA_OR_ROW_END;
            break;
        }

        case MARKET_DATA_KLP_EXPECT_FIELD_COMMA_OR_ROW_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->field_index++;
                if (p->field_index >= 12)
                {
                    p->state = MARKET_DATA_KLP_ERROR;
                    return MARKET_DATA_ERR_PARSE;
                }
                p->state = MARKET_DATA_KLP_EXPECT_FIELD_VALUE;
            }
            else if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                if (p->field_index != 11)
                {
                    p->state = MARKET_DATA_KLP_ERROR;
                    return MARKET_DATA_ERR_PARSE;
                }
                p->out_count++;
                p->state = MARKET_DATA_KLP_EXPECT_ROW_COMMA_OR_OUTER_END;
            }
            else
            {
                p->state = MARKET_DATA_KLP_ERROR;
                return MARKET_DATA_ERR_PARSE;
            }
            break;

        case MARKET_DATA_KLP_EXPECT_ROW_COMMA_OR_OUTER_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_KLP_EXPECT_ROW_START_OR_OUTER_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                p->state = MARKET_DATA_KLP_DONE;
            }
            else
            {
                p->state = MARKET_DATA_KLP_ERROR;
                return MARKET_DATA_ERR_PARSE;
            }
            break;

        case MARKET_DATA_KLP_DONE:
        case MARKET_DATA_KLP_ERROR:
            break;
        }
    }
    return MARKET_DATA_OK;
}

market_data_err_t market_data_klines_parser_finish(const market_data_klines_parser_t *p, uint16_t *out_count)
{
    if (p->state != MARKET_DATA_KLP_DONE)
    {
        return MARKET_DATA_ERR_PARSE;
    }
    if (out_count != NULL)
    {
        *out_count = p->out_count;
    }
    return MARKET_DATA_OK;
}
