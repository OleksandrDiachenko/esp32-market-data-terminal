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
extern "C"
{
#endif

#define SETTINGS_DISPLAY_MAGIC 0x53444953u // 'SDIS'
#define SETTINGS_DISPLAY_VERSION 2u        // bumped: added night mode fields (Settings > Display)

#define SETTINGS_SYMBOLS_MAGIC 0x53534d42u // 'SSMB'
#define SETTINGS_SYMBOLS_VERSION 2u        // bumped alongside SETTINGS_MAX_WATCHLIST - layout size changed
#define SETTINGS_SYMBOL_MAX_LEN 15
#define SETTINGS_MAX_WATCHLIST 10 // bumped from 8 - see docs/decisions/0007-watchlist-management.md

#define SETTINGS_LOCALE_MAGIC 0x534c4f43u // 'SLOC'
#define SETTINGS_LOCALE_VERSION 3u        // bumped: added tz_label (Settings > Time > Time zones)
#define SETTINGS_POSIX_TZ_MAX_LEN 47      // e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3"
#define SETTINGS_DATE_FORMAT_MAX_LEN 15   // e.g. "%b %d, %Y" - longest of the built-in date format options
#define SETTINGS_TZ_LABEL_MAX_LEN 31      // "Zone/City", e.g. "America/Buenos Aires"

#define SETTINGS_API_REGION_MAGIC 0x53415247u // 'SARG'
#define SETTINGS_API_REGION_VERSION                                                                                    \
    2u // bumped: added region_source - see docs/decisions/0009-regional-server-auto-selection.md

    typedef struct
    {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t crc32;
        uint8_t brightness_percent; // 1-100, default 80
        uint8_t night_mode_enabled; // 0/1, default 0
        uint8_t night_start_hour;   // 0-23, default 22
        uint8_t night_start_minute; // 0-59, default 0
        uint8_t night_end_hour;     // 0-23, default 7
        uint8_t night_end_minute;   // 0-59, default 0
        uint8_t reserved2[2];       // pad to 4-byte alignment
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
        char date_format[SETTINGS_DATE_FORMAT_MAX_LEN + 1]; // strftime() pattern, e.g. "%d %b %Y"
        // "Zone/City" label for the exact Time zones selection, e.g.
        // "Europe/Kyiv" - several cities can share one posix_tz DST rule (every
        // CET/CEST city in Europe, for instance), so posix_tz alone can't tell
        // which one the user actually picked. Empty until a selection is made.
        char tz_label[SETTINGS_TZ_LABEL_MAX_LEN + 1];
        bool time_24h;
        uint8_t reserved2[3];
    } locale_settings_t;

    // Which Binance REST host to use. Regulatory split, not a locale/timezone
    // concern, so it is its own domain rather than folded into locale_settings_t.
    typedef enum
    {
        SETTINGS_API_REGION_INTL = 0, // https://api.binance.com
        SETTINGS_API_REGION_US = 1,   // https://api.binance.us
    } settings_api_region_t;

    // Whether `region` was last set by the tz_label -> region auto-mapping
    // (see settings_api_region_map.h) or by an explicit user choice on the
    // Settings > Time > Region screen. A manual choice must not be silently
    // overwritten the next time the user picks a different time zone - see
    // docs/decisions/0009-regional-server-auto-selection.md.
    typedef enum
    {
        SETTINGS_API_REGION_SOURCE_AUTO = 0,
        SETTINGS_API_REGION_SOURCE_MANUAL = 1,
    } settings_api_region_source_t;

    typedef struct
    {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t crc32;
        uint8_t region;        // settings_api_region_t, stored fixed-width on disk
        uint8_t region_source; // settings_api_region_source_t, stored fixed-width on disk
        uint8_t reserved2[2];  // pad to 4-byte alignment
    } api_region_settings_t;

#define SETTINGS_DISCLAIMER_MAGIC 0x5344434cu // 'SDCL'
#define SETTINGS_DISCLAIMER_VERSION 1u
#define SETTINGS_ACKED_FW_VERSION_MAX_LEN 31 // esp_app_desc_t.version is char[32]

    // Tracks the firmware version string the user last accepted the first-run
    // disclaimer for. The disclaimer re-shows whenever the running firmware
    // version differs from acked_fw_version - see
    // settings_disclaimer_should_show() - so it naturally re-appears after every
    // OTA update (the version string always changes) and stays hidden across
    // plain reboots (it doesn't). A disclaimer *text* change can only ship
    // inside a firmware update too, so this one field covers both cases without
    // a separate "disclaimer text version".
    typedef struct
    {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t crc32;
        char acked_fw_version[SETTINGS_ACKED_FW_VERSION_MAX_LEN + 1];
    } disclaimer_settings_t;

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

    void settings_api_region_init_default(api_region_settings_t *out);
    void settings_api_region_seal(api_region_settings_t *db);
    settings_codec_status_t settings_api_region_validate(const api_region_settings_t *db);

    void settings_disclaimer_init_default(disclaimer_settings_t *out);
    void settings_disclaimer_seal(disclaimer_settings_t *db);
    settings_codec_status_t settings_disclaimer_validate(const disclaimer_settings_t *db);

    // Pure decision helper: true when the disclaimer has not yet been accepted
    // for the currently running firmware version (empty/never-acknowledged, or
    // acknowledged for a different version - e.g. an OTA update just landed).
    // running must be non-empty; acked may be empty (never acknowledged).
    bool settings_disclaimer_should_show(const char *acked, const char *running);

#ifdef __cplusplus
}
#endif
