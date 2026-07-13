#pragma once

// Pure C, host-compilable: no ESP-IDF includes. Defines the on-disk blob
// format for the Wi-Fi profile store and its CRC/validation.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_PROFILE_MAX_PROFILES 12
#define WIFI_PROFILE_SSID_MAX 32
#define WIFI_PROFILE_PASSWORD_MAX 64

#define WIFI_PROFILE_DB_MAGIC 0x57464347u // 'WFCG'
#define WIFI_PROFILE_DB_VERSION 1u

#define WIFI_PROFILE_FLAG_BLOCKED 0x01u

    typedef struct
    {
        char ssid[WIFI_PROFILE_SSID_MAX + 1];
        char password[WIFI_PROFILE_PASSWORD_MAX + 1];
        uint8_t flags;
        uint8_t reserved[3];
    } wifi_profile_rec_t;

    typedef struct
    {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t crc32;
        uint8_t count;
        uint8_t padding[3];
        char last_success_ssid[WIFI_PROFILE_SSID_MAX + 1];
        wifi_profile_rec_t records[WIFI_PROFILE_MAX_PROFILES];
    } wifi_profile_db_t;

    typedef enum
    {
        WIFI_PROFILE_CODEC_OK = 0,
        WIFI_PROFILE_CODEC_BAD_MAGIC,
        WIFI_PROFILE_CODEC_BAD_VERSION,
        WIFI_PROFILE_CODEC_BAD_CRC,
        WIFI_PROFILE_CODEC_BAD_COUNT,
    } wifi_profile_codec_status_t;

    uint32_t wifi_profile_codec_crc32(const void *data, size_t len);

    // Zeroes the db and stamps magic/version/count=0.
    void wifi_profile_codec_init_empty(wifi_profile_db_t *db);

    // Recomputes db->crc32 over the whole struct (with the crc field itself
    // treated as zero). Call after any mutation, before persisting.
    void wifi_profile_codec_seal(wifi_profile_db_t *db);

    // Validates magic/version/count bounds and the CRC. Does not seal.
    wifi_profile_codec_status_t wifi_profile_codec_validate(const wifi_profile_db_t *db);

#ifdef __cplusplus
}
#endif
