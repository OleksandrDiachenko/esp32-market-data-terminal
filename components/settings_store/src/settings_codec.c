#include "settings_codec.h"

#include <string.h>

// Every domain record is small (largest today is symbol_settings_t at well
// under 256 bytes); the seal/validate scratch copy is bounded by this so it
// can live on the stack instead of requiring a VLA or heap allocation.
#define SETTINGS_CODEC_MAX_RECORD_SIZE 256

// Fixed by the shared header convention documented in settings_codec.h.
#define SETTINGS_CODEC_MAGIC_OFFSET 0
#define SETTINGS_CODEC_VERSION_OFFSET 4

uint32_t settings_codec_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

void settings_codec_seal(void *record, size_t record_size, size_t crc_offset, uint32_t magic, uint16_t version)
{
    uint8_t *bytes = (uint8_t *)record;
    memcpy(bytes + SETTINGS_CODEC_MAGIC_OFFSET, &magic, sizeof(magic));
    memcpy(bytes + SETTINGS_CODEC_VERSION_OFFSET, &version, sizeof(version));

    // CRC is computed with the crc32 field itself treated as zero, so a
    // scratch copy is used rather than mutating the record in place.
    uint8_t scratch[SETTINGS_CODEC_MAX_RECORD_SIZE];
    memcpy(scratch, record, record_size);
    memset(scratch + crc_offset, 0, sizeof(uint32_t));

    uint32_t crc = settings_codec_crc32(scratch, record_size);
    memcpy(bytes + crc_offset, &crc, sizeof(crc));
}

settings_codec_status_t settings_codec_validate(const void *record, size_t record_size, size_t crc_offset,
                                                 uint32_t magic, uint16_t version)
{
    const uint8_t *bytes = (const uint8_t *)record;

    uint32_t got_magic;
    uint16_t got_version;
    uint32_t got_crc;
    memcpy(&got_magic, bytes + SETTINGS_CODEC_MAGIC_OFFSET, sizeof(got_magic));
    memcpy(&got_version, bytes + SETTINGS_CODEC_VERSION_OFFSET, sizeof(got_version));
    memcpy(&got_crc, bytes + crc_offset, sizeof(got_crc));

    if (got_magic != magic)
    {
        return SETTINGS_CODEC_BAD_MAGIC;
    }
    if (got_version != version)
    {
        return SETTINGS_CODEC_BAD_VERSION;
    }

    uint8_t scratch[SETTINGS_CODEC_MAX_RECORD_SIZE];
    memcpy(scratch, record, record_size);
    memset(scratch + crc_offset, 0, sizeof(uint32_t));
    uint32_t expected = settings_codec_crc32(scratch, record_size);

    if (expected != got_crc)
    {
        return SETTINGS_CODEC_BAD_CRC;
    }

    return SETTINGS_CODEC_OK;
}

_Static_assert(sizeof(display_settings_t) <= SETTINGS_CODEC_MAX_RECORD_SIZE, "display_settings_t exceeds codec scratch buffer");
_Static_assert(sizeof(symbol_settings_t) <= SETTINGS_CODEC_MAX_RECORD_SIZE, "symbol_settings_t exceeds codec scratch buffer");
_Static_assert(sizeof(locale_settings_t) <= SETTINGS_CODEC_MAX_RECORD_SIZE, "locale_settings_t exceeds codec scratch buffer");
_Static_assert(sizeof(api_region_settings_t) <= SETTINGS_CODEC_MAX_RECORD_SIZE, "api_region_settings_t exceeds codec scratch buffer");

void settings_display_init_default(display_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = SETTINGS_DISPLAY_MAGIC;
    out->version = SETTINGS_DISPLAY_VERSION;
    out->brightness_percent = 80;
}

void settings_symbols_init_default(symbol_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = SETTINGS_SYMBOLS_MAGIC;
    out->version = SETTINGS_SYMBOLS_VERSION;
    out->count = 0;
}

void settings_locale_init_default(locale_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = SETTINGS_LOCALE_MAGIC;
    out->version = SETTINGS_LOCALE_VERSION;
    strncpy(out->posix_tz, "UTC0", SETTINGS_POSIX_TZ_MAX_LEN);
    strncpy(out->date_format, "%a %d %b", SETTINGS_DATE_FORMAT_MAX_LEN);
    out->time_24h = true;
}

void settings_display_seal(display_settings_t *db)
{
    settings_codec_seal(db, sizeof(*db), offsetof(display_settings_t, crc32), SETTINGS_DISPLAY_MAGIC,
                         SETTINGS_DISPLAY_VERSION);
}

settings_codec_status_t settings_display_validate(const display_settings_t *db)
{
    settings_codec_status_t status = settings_codec_validate(db, sizeof(*db), offsetof(display_settings_t, crc32),
                                                               SETTINGS_DISPLAY_MAGIC, SETTINGS_DISPLAY_VERSION);
    if (status != SETTINGS_CODEC_OK)
    {
        return status;
    }
    if (db->brightness_percent < 1 || db->brightness_percent > 100)
    {
        return SETTINGS_CODEC_BAD_RANGE;
    }
    return SETTINGS_CODEC_OK;
}

void settings_symbols_seal(symbol_settings_t *db)
{
    settings_codec_seal(db, sizeof(*db), offsetof(symbol_settings_t, crc32), SETTINGS_SYMBOLS_MAGIC,
                         SETTINGS_SYMBOLS_VERSION);
}

settings_codec_status_t settings_symbols_validate(const symbol_settings_t *db)
{
    settings_codec_status_t status = settings_codec_validate(db, sizeof(*db), offsetof(symbol_settings_t, crc32),
                                                               SETTINGS_SYMBOLS_MAGIC, SETTINGS_SYMBOLS_VERSION);
    if (status != SETTINGS_CODEC_OK)
    {
        return status;
    }
    if (db->count > SETTINGS_MAX_WATCHLIST)
    {
        return SETTINGS_CODEC_BAD_RANGE;
    }
    return SETTINGS_CODEC_OK;
}

void settings_locale_seal(locale_settings_t *db)
{
    db->posix_tz[SETTINGS_POSIX_TZ_MAX_LEN] = '\0';
    db->date_format[SETTINGS_DATE_FORMAT_MAX_LEN] = '\0';
    db->tz_label[SETTINGS_TZ_LABEL_MAX_LEN] = '\0';
    settings_codec_seal(db, sizeof(*db), offsetof(locale_settings_t, crc32), SETTINGS_LOCALE_MAGIC,
                         SETTINGS_LOCALE_VERSION);
}

settings_codec_status_t settings_locale_validate(const locale_settings_t *db)
{
    return settings_codec_validate(db, sizeof(*db), offsetof(locale_settings_t, crc32), SETTINGS_LOCALE_MAGIC,
                                    SETTINGS_LOCALE_VERSION);
}

void settings_api_region_init_default(api_region_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = SETTINGS_API_REGION_MAGIC;
    out->version = SETTINGS_API_REGION_VERSION;
    out->region = SETTINGS_API_REGION_INTL;
}

void settings_api_region_seal(api_region_settings_t *db)
{
    settings_codec_seal(db, sizeof(*db), offsetof(api_region_settings_t, crc32), SETTINGS_API_REGION_MAGIC,
                         SETTINGS_API_REGION_VERSION);
}

settings_codec_status_t settings_api_region_validate(const api_region_settings_t *db)
{
    settings_codec_status_t status = settings_codec_validate(
        db, sizeof(*db), offsetof(api_region_settings_t, crc32), SETTINGS_API_REGION_MAGIC, SETTINGS_API_REGION_VERSION);
    if (status != SETTINGS_CODEC_OK)
    {
        return status;
    }
    if (db->region != SETTINGS_API_REGION_INTL && db->region != SETTINGS_API_REGION_US)
    {
        return SETTINGS_CODEC_BAD_RANGE;
    }
    return SETTINGS_CODEC_OK;
}
