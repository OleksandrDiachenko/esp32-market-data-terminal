#include "test_common.h"
#include "settings_codec.h"

static void test_roundtrip(void)
{
    display_settings_t cfg;
    settings_display_init_default(&cfg);
    cfg.brightness_percent = 42;

    settings_display_seal(&cfg);

    CHECK(settings_display_validate(&cfg) == SETTINGS_CODEC_OK);
}

static void test_bad_crc_rejected(void)
{
    display_settings_t cfg;
    settings_display_init_default(&cfg);
    settings_display_seal(&cfg);
    cfg.crc32 ^= 0xFFFFFFFFu;

    CHECK(settings_display_validate(&cfg) == SETTINGS_CODEC_BAD_CRC);
}

static void test_bad_magic_rejected(void)
{
    display_settings_t cfg;
    settings_display_init_default(&cfg);
    settings_display_seal(&cfg);
    cfg.magic = 0xDEADBEEFu;

    CHECK(settings_display_validate(&cfg) == SETTINGS_CODEC_BAD_MAGIC);
}

static void test_bad_version_rejected(void)
{
    display_settings_t cfg;
    settings_display_init_default(&cfg);
    settings_display_seal(&cfg);
    cfg.version = 99;

    CHECK(settings_display_validate(&cfg) == SETTINGS_CODEC_BAD_VERSION);
}

static void test_out_of_range_brightness_rejected(void)
{
    display_settings_t cfg;
    settings_display_init_default(&cfg);
    cfg.brightness_percent = 0;
    settings_display_seal(&cfg);

    CHECK(settings_display_validate(&cfg) == SETTINGS_CODEC_BAD_RANGE);
}

int main(void)
{
    test_roundtrip();
    test_bad_crc_rejected();
    test_bad_magic_rejected();
    test_bad_version_rejected();
    test_out_of_range_brightness_rejected();
    return test_summary("settings_codec_display");
}
