#include "ota_client_json.h"

#include <string.h>

// A full JSON parser (or Phase 7's streaming scanner, built for
// arbitrarily-large/chunk-split klines responses) would be overkill here:
// ota_client.c reads the /releases/latest response into one small bounded
// buffer (release metadata is a few KB, capped well before the point where
// a long release-notes "body" field could matter), and the only field this
// component needs out of it is "tag_name". A plain substring search over
// that already-in-memory buffer is simpler and sufficient.
static const char *find_field(const char *json, size_t json_len, const char *key_with_colon)
{
    size_t key_len = strlen(key_with_colon);
    if (json_len < key_len)
    {
        return NULL;
    }
    for (size_t i = 0; i + key_len <= json_len; i++)
    {
        if (memcmp(json + i, key_with_colon, key_len) == 0)
        {
            return json + i + key_len;
        }
    }
    return NULL;
}

ota_client_err_t ota_client_extract_tag_name(const char *json, size_t json_len, char *out, size_t out_capacity)
{
    if (json == NULL || out == NULL || out_capacity == 0)
    {
        return OTA_CLIENT_ERR_INVALID_ARG;
    }

    // GitHub's actual response has no space after the colon
    // (`"tag_name":"..."`), but tolerate one for robustness against a
    // differently-formatted (e.g. pretty-printed) response.
    const char *value = find_field(json, json_len, "\"tag_name\":\"");
    if (value == NULL)
    {
        value = find_field(json, json_len, "\"tag_name\": \"");
    }
    if (value == NULL)
    {
        return OTA_CLIENT_ERR_PARSE;
    }

    size_t remaining = json_len - (size_t)(value - json);
    size_t i = 0;
    for (; i < remaining && value[i] != '"'; i++)
    {
        if (i + 1 >= out_capacity)
        {
            return OTA_CLIENT_ERR_PARSE; // value doesn't fit
        }
        out[i] = value[i];
    }
    if (i == remaining)
    {
        return OTA_CLIENT_ERR_PARSE; // unterminated string
    }
    out[i] = '\0';
    return OTA_CLIENT_OK;
}
