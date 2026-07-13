#include "test_common.h"
#include "ota_client_json.h"

static void test_extracts_tag_name_no_space(void)
{
    const char *json = "{\"url\":\"...\",\"tag_name\":\"0.10.0\",\"name\":\"v0.10.0\"}";
    char out[OTA_CLIENT_TAG_MAX_LEN];
    ota_client_err_t err = ota_client_extract_tag_name(json, strlen(json), out, sizeof(out));
    CHECK(err == OTA_CLIENT_OK);
    CHECK_STREQ(out, "0.10.0");
}

static void test_extracts_tag_name_with_space(void)
{
    const char *json = "{\n  \"tag_name\": \"0.11.0\"\n}";
    char out[OTA_CLIENT_TAG_MAX_LEN];
    ota_client_err_t err = ota_client_extract_tag_name(json, strlen(json), out, sizeof(out));
    CHECK(err == OTA_CLIENT_OK);
    CHECK_STREQ(out, "0.11.0");
}

static void test_realistic_github_release_payload(void)
{
    const char *json = "{\"url\":\"https://api.github.com/repos/o/r/releases/1\","
                       "\"html_url\":\"https://github.com/o/r/releases/tag/0.10.0\","
                       "\"id\":1,\"tag_name\":\"0.10.0\",\"target_commitish\":\"main\","
                       "\"name\":\"0.10.0\",\"draft\":false,\"prerelease\":false,"
                       "\"assets\":[{\"browser_download_url\":\"https://github.com/o/r/releases/download/0.10.0/"
                       "esp32-market-data-terminal.bin\"}]}";
    char out[OTA_CLIENT_TAG_MAX_LEN];
    ota_client_err_t err = ota_client_extract_tag_name(json, strlen(json), out, sizeof(out));
    CHECK(err == OTA_CLIENT_OK);
    CHECK_STREQ(out, "0.10.0");
}

static void test_missing_field_returns_parse_error(void)
{
    const char *json = "{\"name\":\"0.10.0\"}";
    char out[OTA_CLIENT_TAG_MAX_LEN];
    CHECK(ota_client_extract_tag_name(json, strlen(json), out, sizeof(out)) == OTA_CLIENT_ERR_PARSE);
}

static void test_unterminated_string_returns_parse_error(void)
{
    const char *json = "{\"tag_name\":\"0.10.0";
    char out[OTA_CLIENT_TAG_MAX_LEN];
    CHECK(ota_client_extract_tag_name(json, strlen(json), out, sizeof(out)) == OTA_CLIENT_ERR_PARSE);
}

static void test_value_too_large_for_out_buffer(void)
{
    const char *json = "{\"tag_name\":\"0.10.0-a-very-long-prerelease-suffix-that-does-not-fit\"}";
    char out[8];
    CHECK(ota_client_extract_tag_name(json, strlen(json), out, sizeof(out)) == OTA_CLIENT_ERR_PARSE);
}

static void test_empty_json_returns_parse_error(void)
{
    const char *json = "";
    char out[OTA_CLIENT_TAG_MAX_LEN];
    CHECK(ota_client_extract_tag_name(json, strlen(json), out, sizeof(out)) == OTA_CLIENT_ERR_PARSE);
}

static void test_rejects_invalid_args(void)
{
    char out[OTA_CLIENT_TAG_MAX_LEN];
    CHECK(ota_client_extract_tag_name(NULL, 0, out, sizeof(out)) == OTA_CLIENT_ERR_INVALID_ARG);
    CHECK(ota_client_extract_tag_name("{}", 2, NULL, sizeof(out)) == OTA_CLIENT_ERR_INVALID_ARG);
    CHECK(ota_client_extract_tag_name("{}", 2, out, 0) == OTA_CLIENT_ERR_INVALID_ARG);
}

int main(void)
{
    test_extracts_tag_name_no_space();
    test_extracts_tag_name_with_space();
    test_realistic_github_release_payload();
    test_missing_field_returns_parse_error();
    test_unterminated_string_returns_parse_error();
    test_value_too_large_for_out_buffer();
    test_empty_json_returns_parse_error();
    test_rejects_invalid_args();
    return test_summary("ota_client_json");
}
