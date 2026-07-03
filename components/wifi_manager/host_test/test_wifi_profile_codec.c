#include "test_common.h"
#include "wifi_profile_codec.h"

static void test_roundtrip(void)
{
    wifi_profile_db_t db;
    wifi_profile_codec_init_empty(&db);
    db.count = 1;
    strncpy(db.records[0].ssid, "Home", WIFI_PROFILE_SSID_MAX);
    strncpy(db.records[0].password, "hunter2", WIFI_PROFILE_PASSWORD_MAX);
    strncpy(db.last_success_ssid, "Home", WIFI_PROFILE_SSID_MAX);

    wifi_profile_codec_seal(&db);

    CHECK(wifi_profile_codec_validate(&db) == WIFI_PROFILE_CODEC_OK);
}

static void test_bad_crc_rejected(void)
{
    wifi_profile_db_t db;
    wifi_profile_codec_init_empty(&db);
    wifi_profile_codec_seal(&db);
    db.crc32 ^= 0xFFFFFFFFu;

    CHECK(wifi_profile_codec_validate(&db) == WIFI_PROFILE_CODEC_BAD_CRC);
}

static void test_bad_magic_rejected(void)
{
    wifi_profile_db_t db;
    wifi_profile_codec_init_empty(&db);
    wifi_profile_codec_seal(&db);
    db.magic = 0xDEADBEEFu;

    CHECK(wifi_profile_codec_validate(&db) == WIFI_PROFILE_CODEC_BAD_MAGIC);
}

static void test_bad_version_rejected(void)
{
    wifi_profile_db_t db;
    wifi_profile_codec_init_empty(&db);
    wifi_profile_codec_seal(&db);
    db.version = 99;

    CHECK(wifi_profile_codec_validate(&db) == WIFI_PROFILE_CODEC_BAD_VERSION);
}

static void test_bad_count_rejected(void)
{
    wifi_profile_db_t db;
    wifi_profile_codec_init_empty(&db);
    wifi_profile_codec_seal(&db);
    db.count = WIFI_PROFILE_MAX_PROFILES + 1;

    CHECK(wifi_profile_codec_validate(&db) == WIFI_PROFILE_CODEC_BAD_COUNT);
}

int main(void)
{
    test_roundtrip();
    test_bad_crc_rejected();
    test_bad_magic_rejected();
    test_bad_version_rejected();
    test_bad_count_rejected();
    return test_summary("wifi_profile_codec");
}
