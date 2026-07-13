#include "test_common.h"
#include "market_data_json_scanner.h"

#define MAX_CAPTURED 32

typedef struct
{
    market_data_json_token_kind_t kind;
    char text[64];
    double number;
} captured_token_t;

static int collect_all(market_data_json_scanner_t *sc, const char *buf, size_t len, captured_token_t *out, int max_out)
{
    int count = 0;
    while (len > 0 && count < max_out)
    {
        double num = 0;
        market_data_json_token_kind_t tok = market_data_json_scanner_next(sc, &buf, &len, &num);
        if (tok == MARKET_DATA_JSON_TOK_NONE)
        {
            break;
        }
        out[count].kind = tok;
        out[count].number = num;
        out[count].text[0] = '\0';
        if (tok == MARKET_DATA_JSON_TOK_STRING)
        {
            strncpy(out[count].text, market_data_json_scanner_text(sc), sizeof(out[count].text) - 1);
        }
        count++;
        if (tok == MARKET_DATA_JSON_TOK_ERROR)
        {
            break;
        }
    }
    return count;
}

static void test_structural_chars(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    captured_token_t tokens[8];
    int n = collect_all(&sc, "[]{},:", 6, tokens, 8);
    CHECK(n == 6);
    CHECK(tokens[0].kind == MARKET_DATA_JSON_TOK_ARRAY_START);
    CHECK(tokens[1].kind == MARKET_DATA_JSON_TOK_ARRAY_END);
    CHECK(tokens[2].kind == MARKET_DATA_JSON_TOK_OBJECT_START);
    CHECK(tokens[3].kind == MARKET_DATA_JSON_TOK_OBJECT_END);
    CHECK(tokens[4].kind == MARKET_DATA_JSON_TOK_COMMA);
    CHECK(tokens[5].kind == MARKET_DATA_JSON_TOK_COLON);
}

static void test_simple_string(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    const char *input = "\"hello\"";
    captured_token_t tokens[4];
    int n = collect_all(&sc, input, strlen(input), tokens, 4);
    CHECK(n == 1);
    CHECK(tokens[0].kind == MARKET_DATA_JSON_TOK_STRING);
    CHECK_STREQ(tokens[0].text, "hello");
}

static void test_string_with_escapes(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    const char *input = "\"a\\nb\\tc\\\"d\\\\e\"";
    captured_token_t tokens[4];
    int n = collect_all(&sc, input, strlen(input), tokens, 4);
    CHECK(n == 1);
    CHECK(tokens[0].kind == MARKET_DATA_JSON_TOK_STRING);
    CHECK_STREQ(tokens[0].text, "a\nb\tc\"d\\e");
}

static void test_rejects_unicode_escape(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    const char *input = "\"\\u0041\"";
    captured_token_t tokens[4];
    int n = collect_all(&sc, input, strlen(input), tokens, 4);
    CHECK(n == 1);
    CHECK(tokens[0].kind == MARKET_DATA_JSON_TOK_ERROR);
}

static void test_numbers(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    const char *input = "123,-12.5,4.2e3]";
    captured_token_t tokens[8];
    int n = collect_all(&sc, input, strlen(input), tokens, 8);
    CHECK(n == 6);
    CHECK(tokens[0].kind == MARKET_DATA_JSON_TOK_NUMBER);
    CHECK(tokens[0].number == 123.0);
    CHECK(tokens[1].kind == MARKET_DATA_JSON_TOK_COMMA);
    CHECK(tokens[2].kind == MARKET_DATA_JSON_TOK_NUMBER);
    CHECK(tokens[2].number == -12.5);
    CHECK(tokens[3].kind == MARKET_DATA_JSON_TOK_COMMA);
    CHECK(tokens[4].kind == MARKET_DATA_JSON_TOK_NUMBER);
    CHECK(tokens[4].number == 4200.0);
    CHECK(tokens[5].kind == MARKET_DATA_JSON_TOK_ARRAY_END);
}

static void test_literals(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    const char *input = "true,false,null]";
    captured_token_t tokens[8];
    int n = collect_all(&sc, input, strlen(input), tokens, 8);
    CHECK(n == 6);
    CHECK(tokens[0].kind == MARKET_DATA_JSON_TOK_TRUE);
    CHECK(tokens[2].kind == MARKET_DATA_JSON_TOK_FALSE);
    CHECK(tokens[4].kind == MARKET_DATA_JSON_TOK_NULL);
}

static void test_rejects_bad_literal(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    const char *input = "tru ";
    captured_token_t tokens[4];
    int n = collect_all(&sc, input, strlen(input), tokens, 4);
    CHECK(n == 1);
    CHECK(tokens[0].kind == MARKET_DATA_JSON_TOK_ERROR);
}

static void test_number_split_across_feed_calls(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);

    const char *chunk1 = "12";
    const char *buf1 = chunk1;
    size_t len1 = strlen(chunk1);
    double num = 0;
    market_data_json_token_kind_t tok = market_data_json_scanner_next(&sc, &buf1, &len1, &num);
    CHECK(tok == MARKET_DATA_JSON_TOK_NONE);
    CHECK(len1 == 0);

    const char *chunk2 = "3,";
    const char *buf2 = chunk2;
    size_t len2 = strlen(chunk2);
    tok = market_data_json_scanner_next(&sc, &buf2, &len2, &num);
    CHECK(tok == MARKET_DATA_JSON_TOK_NUMBER);
    CHECK(num == 123.0);

    tok = market_data_json_scanner_next(&sc, &buf2, &len2, &num);
    CHECK(tok == MARKET_DATA_JSON_TOK_COMMA);
}

static void test_string_split_byte_by_byte(void)
{
    market_data_json_scanner_t sc;
    market_data_json_scanner_init(&sc);
    const char *input = "\"BTCUSDT\"";
    size_t total = strlen(input);

    market_data_json_token_kind_t last_tok = MARKET_DATA_JSON_TOK_NONE;
    for (size_t i = 0; i < total; i++)
    {
        const char *buf = &input[i];
        size_t len = 1;
        double num = 0;
        last_tok = market_data_json_scanner_next(&sc, &buf, &len, &num);
        if (i + 1 < total)
        {
            CHECK(last_tok == MARKET_DATA_JSON_TOK_NONE);
        }
    }
    CHECK(last_tok == MARKET_DATA_JSON_TOK_STRING);
    CHECK_STREQ(market_data_json_scanner_text(&sc), "BTCUSDT");
}

int main(void)
{
    test_structural_chars();
    test_simple_string();
    test_string_with_escapes();
    test_rejects_unicode_escape();
    test_numbers();
    test_literals();
    test_rejects_bad_literal();
    test_number_split_across_feed_calls();
    test_string_split_byte_by_byte();
    return test_summary("market_data_json_scanner");
}
