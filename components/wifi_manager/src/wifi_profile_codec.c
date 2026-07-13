#include "wifi_profile_codec.h"

#include <string.h>

uint32_t wifi_profile_codec_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

void wifi_profile_codec_init_empty(wifi_profile_db_t *db)
{
    memset(db, 0, sizeof(*db));
    db->magic = WIFI_PROFILE_DB_MAGIC;
    db->version = WIFI_PROFILE_DB_VERSION;
    db->count = 0;
}

void wifi_profile_codec_seal(wifi_profile_db_t *db)
{
    db->magic = WIFI_PROFILE_DB_MAGIC;
    db->version = WIFI_PROFILE_DB_VERSION;

    // CRC is computed with the crc32 field itself treated as zero, so a
    // scratch copy is used rather than mutating db in place.
    const size_t crc_offset = offsetof(wifi_profile_db_t, crc32);
    uint8_t scratch[sizeof(wifi_profile_db_t)];
    memcpy(scratch, db, sizeof(wifi_profile_db_t));
    memset(scratch + crc_offset, 0, sizeof(db->crc32));

    db->crc32 = wifi_profile_codec_crc32(scratch, sizeof(wifi_profile_db_t));
}

wifi_profile_codec_status_t wifi_profile_codec_validate(const wifi_profile_db_t *db)
{
    if (db->magic != WIFI_PROFILE_DB_MAGIC)
    {
        return WIFI_PROFILE_CODEC_BAD_MAGIC;
    }
    if (db->version != WIFI_PROFILE_DB_VERSION)
    {
        return WIFI_PROFILE_CODEC_BAD_VERSION;
    }
    if (db->count > WIFI_PROFILE_MAX_PROFILES)
    {
        return WIFI_PROFILE_CODEC_BAD_COUNT;
    }

    const size_t crc_offset = offsetof(wifi_profile_db_t, crc32);
    uint8_t copy[sizeof(wifi_profile_db_t)];
    memcpy(copy, db, sizeof(wifi_profile_db_t));
    memset(copy + crc_offset, 0, sizeof(db->crc32));
    uint32_t expected = wifi_profile_codec_crc32(copy, sizeof(wifi_profile_db_t));

    if (expected != db->crc32)
    {
        return WIFI_PROFILE_CODEC_BAD_CRC;
    }

    return WIFI_PROFILE_CODEC_OK;
}
