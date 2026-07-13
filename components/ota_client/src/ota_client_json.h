#pragma once

// Minimal, host-testable extraction of a single field ("tag_name") from a
// GitHub /releases/latest JSON response already read into memory. Not a
// general JSON parser (see ota_client_json.c for why a full parser or
// Phase 7's streaming-scanner approach isn't needed here): the response is
// read into a small bounded buffer by ota_client.c, and this just finds
// `"tag_name":"<value>"` within it.

#include <stddef.h>

#include "ota_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Finds `"tag_name":"<value>"` in json[0..json_len) and copies <value> into
    // out (NUL-terminated). Returns OTA_CLIENT_ERR_PARSE if the field is
    // missing, unterminated, or its value doesn't fit in out_capacity.
    ota_client_err_t ota_client_extract_tag_name(const char *json, size_t json_len, char *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif
