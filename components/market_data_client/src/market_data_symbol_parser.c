#include "market_data_symbol_parser.h"

#include <string.h>

typedef enum
{
    MARKET_DATA_SYP_LEVEL_ROOT,
    MARKET_DATA_SYP_LEVEL_SYMBOL,
} market_data_syp_level_t;

static market_data_symbol_parser_key_t key_from_text(const char *text, market_data_syp_level_t level)
{
    if (level == MARKET_DATA_SYP_LEVEL_ROOT)
    {
        return strcmp(text, "symbols") == 0 ? MARKET_DATA_SYP_KEY_SYMBOLS : MARKET_DATA_SYP_KEY_OTHER;
    }
    if (strcmp(text, "status") == 0)
    {
        return MARKET_DATA_SYP_KEY_STATUS;
    }
    if (strcmp(text, "permissionSets") == 0)
    {
        return MARKET_DATA_SYP_KEY_PERMISSION_SETS;
    }
    return MARKET_DATA_SYP_KEY_OTHER;
}

// Advances a generic "skip this value" depth counter given the next token.
// Call with *depth == 0 on the value's first token (which may itself
// complete the skip, if it's a scalar). Returns true once the value has
// been fully consumed.
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

void market_data_symbol_parser_init(market_data_symbol_parser_t *p, market_data_symbol_status_t *out_status)
{
    memset(p, 0, sizeof(*p));
    market_data_json_scanner_init(&p->scanner);
    p->state = MARKET_DATA_SYP_EXPECT_ROOT_OBJ_START;
    p->out_status = out_status;
    out_status->is_trading = false;
    out_status->has_spot_permission = false;
}

market_data_err_t market_data_symbol_parser_feed(market_data_symbol_parser_t *p, const char *buf, size_t len)
{
    while (len > 0 && p->state != MARKET_DATA_SYP_DONE && p->state != MARKET_DATA_SYP_ERROR)
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
        case MARKET_DATA_SYP_EXPECT_ROOT_OBJ_START:
            if (tok != MARKET_DATA_JSON_TOK_OBJECT_START)
            {
                goto fail;
            }
            p->state = MARKET_DATA_SYP_EXPECT_ROOT_KEY_OR_END;
            break;

        case MARKET_DATA_SYP_EXPECT_ROOT_KEY_OR_END:
            // OBJECT_END here means the root object closed without ever
            // containing a "symbols" array - always an error for this grammar.
            if (tok != MARKET_DATA_JSON_TOK_STRING)
            {
                goto fail;
            }
            p->pending_key = key_from_text(market_data_json_scanner_text(&p->scanner), MARKET_DATA_SYP_LEVEL_ROOT);
            p->state = MARKET_DATA_SYP_EXPECT_ROOT_COLON;
            break;

        case MARKET_DATA_SYP_EXPECT_ROOT_COLON:
            if (tok != MARKET_DATA_JSON_TOK_COLON)
            {
                goto fail;
            }
            p->state = MARKET_DATA_SYP_EXPECT_ROOT_VALUE;
            break;

        case MARKET_DATA_SYP_EXPECT_ROOT_VALUE:
            if (p->pending_key == MARKET_DATA_SYP_KEY_SYMBOLS)
            {
                if (tok != MARKET_DATA_JSON_TOK_ARRAY_START)
                {
                    goto fail;
                }
                p->state = MARKET_DATA_SYP_EXPECT_SYMBOL0_START_OR_ARR_END;
            }
            else
            {
                p->skip_depth = 0;
                p->state = skip_advance(&p->skip_depth, tok) ? MARKET_DATA_SYP_EXPECT_ROOT_COMMA_OR_END
                                                             : MARKET_DATA_SYP_SKIP_ROOT_VALUE;
            }
            break;

        case MARKET_DATA_SYP_EXPECT_ROOT_COMMA_OR_END:
            if (tok != MARKET_DATA_JSON_TOK_COMMA)
            {
                goto fail; // includes OBJECT_END: root closed, "symbols" never found
            }
            p->state = MARKET_DATA_SYP_EXPECT_ROOT_KEY_OR_END;
            break;

        case MARKET_DATA_SYP_SKIP_ROOT_VALUE:
            if (skip_advance(&p->skip_depth, tok))
            {
                p->state = MARKET_DATA_SYP_EXPECT_ROOT_COMMA_OR_END;
            }
            break;

        case MARKET_DATA_SYP_EXPECT_SYMBOL0_START_OR_ARR_END:
            if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                goto fail; // empty symbols array
            }
            if (tok != MARKET_DATA_JSON_TOK_OBJECT_START)
            {
                goto fail;
            }
            p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_KEY_OR_END;
            break;

        case MARKET_DATA_SYP_EXPECT_SYMBOL_KEY_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (!p->saw_status)
                {
                    goto fail;
                }
                p->state = MARKET_DATA_SYP_DONE;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_STRING)
            {
                goto fail;
            }
            p->pending_key = key_from_text(market_data_json_scanner_text(&p->scanner), MARKET_DATA_SYP_LEVEL_SYMBOL);
            p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_COLON;
            break;

        case MARKET_DATA_SYP_EXPECT_SYMBOL_COLON:
            if (tok != MARKET_DATA_JSON_TOK_COLON)
            {
                goto fail;
            }
            p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_VALUE;
            break;

        case MARKET_DATA_SYP_EXPECT_SYMBOL_VALUE:
            if (p->pending_key == MARKET_DATA_SYP_KEY_STATUS)
            {
                if (tok != MARKET_DATA_JSON_TOK_STRING)
                {
                    goto fail;
                }
                p->out_status->is_trading = strcmp(market_data_json_scanner_text(&p->scanner), "TRADING") == 0;
                p->saw_status = true;
                p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_COMMA_OR_END;
            }
            else if (p->pending_key == MARKET_DATA_SYP_KEY_PERMISSION_SETS)
            {
                if (tok != MARKET_DATA_JSON_TOK_ARRAY_START)
                {
                    goto fail;
                }
                p->state = MARKET_DATA_SYP_EXPECT_PERMSET_START_OR_ARR_END;
            }
            else
            {
                p->skip_depth = 0;
                p->state = skip_advance(&p->skip_depth, tok) ? MARKET_DATA_SYP_EXPECT_SYMBOL_COMMA_OR_END
                                                             : MARKET_DATA_SYP_SKIP_SYMBOL_VALUE;
            }
            break;

        case MARKET_DATA_SYP_EXPECT_SYMBOL_COMMA_OR_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_KEY_OR_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_OBJECT_END)
            {
                if (!p->saw_status)
                {
                    goto fail;
                }
                p->state = MARKET_DATA_SYP_DONE;
            }
            else
            {
                goto fail;
            }
            break;

        case MARKET_DATA_SYP_SKIP_SYMBOL_VALUE:
            if (skip_advance(&p->skip_depth, tok))
            {
                p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_COMMA_OR_END;
            }
            break;

        case MARKET_DATA_SYP_EXPECT_PERMSET_START_OR_ARR_END:
            if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_COMMA_OR_END;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_ARRAY_START)
            {
                goto fail;
            }
            p->state = MARKET_DATA_SYP_EXPECT_PERM_STRING_OR_ARR_END;
            break;

        case MARKET_DATA_SYP_EXPECT_PERM_STRING_OR_ARR_END:
            if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                p->state = MARKET_DATA_SYP_EXPECT_PERMSETS_COMMA_OR_ARR_END;
                break;
            }
            if (tok != MARKET_DATA_JSON_TOK_STRING)
            {
                goto fail;
            }
            if (strcmp(market_data_json_scanner_text(&p->scanner), "SPOT") == 0)
            {
                p->out_status->has_spot_permission = true;
            }
            p->state = MARKET_DATA_SYP_EXPECT_PERM_COMMA_OR_ARR_END;
            break;

        case MARKET_DATA_SYP_EXPECT_PERM_COMMA_OR_ARR_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_SYP_EXPECT_PERM_STRING_OR_ARR_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                p->state = MARKET_DATA_SYP_EXPECT_PERMSETS_COMMA_OR_ARR_END;
            }
            else
            {
                goto fail;
            }
            break;

        case MARKET_DATA_SYP_EXPECT_PERMSETS_COMMA_OR_ARR_END:
            if (tok == MARKET_DATA_JSON_TOK_COMMA)
            {
                p->state = MARKET_DATA_SYP_EXPECT_PERMSET_START_OR_ARR_END;
            }
            else if (tok == MARKET_DATA_JSON_TOK_ARRAY_END)
            {
                p->state = MARKET_DATA_SYP_EXPECT_SYMBOL_COMMA_OR_END;
            }
            else
            {
                goto fail;
            }
            break;

        case MARKET_DATA_SYP_DONE:
        case MARKET_DATA_SYP_ERROR:
            break;
        }
    }
    return MARKET_DATA_OK;

fail:
    p->state = MARKET_DATA_SYP_ERROR;
    return MARKET_DATA_ERR_PARSE;
}

market_data_err_t market_data_symbol_parser_finish(const market_data_symbol_parser_t *p)
{
    return p->state == MARKET_DATA_SYP_DONE ? MARKET_DATA_OK : MARKET_DATA_ERR_PARSE;
}
