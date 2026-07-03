#pragma once

// Pure C, host-compilable: defines the on-disk blob format for each
// settings domain (display / symbols / locale) and their CRC/validation.
// Every record starts with the same header layout:
//   { uint32_t magic; uint16_t version; uint16_t reserved; uint32_t crc32; }
// New fields are appended at the end of a domain struct and the domain's
// *_VERSION bumped; there is no migration engine yet because there is
// nothing to migrate from until a second version exists.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_DISPLAY_MAGIC 0x53444953u // 'SDIS'
#define SETTINGS_DISPLAY_VERSION 1u

#define SETTINGS_SYMBOLS_MAGIC 0x53534d42u // 'SSMB'
#define SETTINGS_SYMBOLS_VERSION 1u
#define SETTINGS_SYMBOL_MAX_LEN 15
#define SETTINGS_MAX_WATCHLIST 8

#define SETTINGS_LOCALE_MAGIC 0x534c4f43u // 'SLOC'
#define SETTINGS_LOCALE_VERSION 1u
#define SETTINGS_POSIX_TZ_MAX_LEN 47 // e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3"

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t crc32;
    uint8_t brightness_percent; // 1-100, default 80
    uint8_t reserved2[3];       // room for theme/timeout once the board
                                // supports more than on/off backlight
} display_settings_t;

typedef struct
{
    char ticker[SETTINGS_SYMBOL_MAX_LEN + 1];
} settings_symbol_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t crc32;
    uint8_t count;
    uint8_t reserved2[3];
    settings_symbol_t symbols[SETTINGS_MAX_WATCHLIST];
} symbol_settings_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t crc32;
    char posix_tz[SETTINGS_POSIX_TZ_MAX_LEN + 1];
    bool time_24h;
    uint8_t reserved2[3];
} locale_settings_t;

typedef enum
{
    SETTINGS_CODEC_OK = 0,
    SETTINGS_CODEC_BAD_MAGIC,
    SETTINGS_CODEC_BAD_VERSION,
    SETTINGS_CODEC_BAD_CRC,
    SETTINGS_CODEC_BAD_RANGE,
} settings_codec_status_t;

uint32_t settings_codec_crc32(const void *data, size_t len);

// Generic seal/validate over a record whose layout starts with the shared
// header above. crc_offset is the byte offset of that record's crc32
// field (always offsetof(<type>, crc32) at the call site).
void settings_codec_seal(void *record, size_t record_size, size_t crc_offset, uint32_t magic, uint16_t version);
settings_codec_status_t settings_codec_validate(const void *record, size_t record_size, size_t crc_offset,
                                                 uint32_t magic, uint16_t version);

// Domain defaults: zero the struct and stamp sane starting values.
void settings_display_init_default(display_settings_t *out);
void settings_symbols_init_default(symbol_settings_t *out);
void settings_locale_init_default(locale_settings_t *out);

// Domain seal/validate: thin wrappers over settings_codec_seal/validate
// plus any domain-specific bounds checks (e.g. brightness range).
void settings_display_seal(display_settings_t *db);
settings_codec_status_t settings_display_validate(const display_settings_t *db);

void settings_symbols_seal(symbol_settings_t *db);
settings_codec_status_t settings_symbols_validate(const symbol_settings_t *db);

void settings_locale_seal(locale_settings_t *db);
settings_codec_status_t settings_locale_validate(const locale_settings_t *db);

#ifdef __cplusplus
}
#endif
