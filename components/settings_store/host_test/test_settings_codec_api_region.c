#include "test_common.h"
#include "settings_codec.h"

static void test_roundtrip(void)
{
    api_region_settings_t cfg;
    settings_api_region_init_default(&cfg);
    cfg.region = SETTINGS_API_REGION_US;
    cfg.region_source = SETTINGS_API_REGION_SOURCE_MANUAL;

    settings_api_region_seal(&cfg);

    CHECK(settings_api_region_validate(&cfg) == SETTINGS_CODEC_OK);
    CHECK(cfg.region == SETTINGS_API_REGION_US);
    CHECK(cfg.region_source == SETTINGS_API_REGION_SOURCE_MANUAL);
}

static void test_default_is_intl_and_auto(void)
{
    api_region_settings_t cfg;
    settings_api_region_init_default(&cfg);
    settings_api_region_seal(&cfg);

    CHECK(settings_api_region_validate(&cfg) == SETTINGS_CODEC_OK);
    CHECK(cfg.region == SETTINGS_API_REGION_INTL);
    CHECK(cfg.region_source == SETTINGS_API_REGION_SOURCE_AUTO);
}

static void test_bad_crc_rejected(void)
{
    api_region_settings_t cfg;
    settings_api_region_init_default(&cfg);
    settings_api_region_seal(&cfg);
    cfg.crc32 ^= 0xFFFFFFFFu;

    CHECK(settings_api_region_validate(&cfg) == SETTINGS_CODEC_BAD_CRC);
}

static void test_bad_magic_rejected(void)
{
    api_region_settings_t cfg;
    settings_api_region_init_default(&cfg);
    settings_api_region_seal(&cfg);
    cfg.magic = 0xDEADBEEFu;

    CHECK(settings_api_region_validate(&cfg) == SETTINGS_CODEC_BAD_MAGIC);
}

static void test_bad_version_rejected(void)
{
    api_region_settings_t cfg;
    settings_api_region_init_default(&cfg);
    settings_api_region_seal(&cfg);
    cfg.version = 99;

    CHECK(settings_api_region_validate(&cfg) == SETTINGS_CODEC_BAD_VERSION);
}

static void test_region_out_of_range_rejected(void)
{
    api_region_settings_t cfg;
    settings_api_region_init_default(&cfg);
    cfg.region = 2;
    settings_api_region_seal(&cfg);

    CHECK(settings_api_region_validate(&cfg) == SETTINGS_CODEC_BAD_RANGE);
}

static void test_region_source_out_of_range_rejected(void)
{
    api_region_settings_t cfg;
    settings_api_region_init_default(&cfg);
    cfg.region_source = 2;
    settings_api_region_seal(&cfg);

    CHECK(settings_api_region_validate(&cfg) == SETTINGS_CODEC_BAD_RANGE);
}

int main(void)
{
    test_roundtrip();
    test_default_is_intl_and_auto();
    test_bad_crc_rejected();
    test_bad_magic_rejected();
    test_bad_version_rejected();
    test_region_out_of_range_rejected();
    test_region_source_out_of_range_rejected();
    return test_summary("settings_codec_api_region");
}
