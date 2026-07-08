#include "test_common.h"
#include "settings_codec.h"

static void test_roundtrip(void)
{
    locale_settings_t cfg;
    settings_locale_init_default(&cfg);
    strncpy(cfg.posix_tz, "EST5EDT,M3.2.0,M11.1.0", SETTINGS_POSIX_TZ_MAX_LEN);
    strncpy(cfg.date_format, "%Y-%m-%d", SETTINGS_DATE_FORMAT_MAX_LEN);
    strncpy(cfg.tz_label, "America/New York", SETTINGS_TZ_LABEL_MAX_LEN);
    cfg.time_24h = false;

    settings_locale_seal(&cfg);

    CHECK(settings_locale_validate(&cfg) == SETTINGS_CODEC_OK);
    CHECK_STREQ(cfg.posix_tz, "EST5EDT,M3.2.0,M11.1.0");
    CHECK_STREQ(cfg.date_format, "%Y-%m-%d");
    CHECK_STREQ(cfg.tz_label, "America/New York");
}

static void test_bad_crc_rejected(void)
{
    locale_settings_t cfg;
    settings_locale_init_default(&cfg);
    settings_locale_seal(&cfg);
    cfg.crc32 ^= 0xFFFFFFFFu;

    CHECK(settings_locale_validate(&cfg) == SETTINGS_CODEC_BAD_CRC);
}

static void test_bad_magic_rejected(void)
{
    locale_settings_t cfg;
    settings_locale_init_default(&cfg);
    settings_locale_seal(&cfg);
    cfg.magic = 0xDEADBEEFu;

    CHECK(settings_locale_validate(&cfg) == SETTINGS_CODEC_BAD_MAGIC);
}

static void test_bad_version_rejected(void)
{
    locale_settings_t cfg;
    settings_locale_init_default(&cfg);
    settings_locale_seal(&cfg);
    cfg.version = 99;

    CHECK(settings_locale_validate(&cfg) == SETTINGS_CODEC_BAD_VERSION);
}

int main(void)
{
    test_roundtrip();
    test_bad_crc_rejected();
    test_bad_magic_rejected();
    test_bad_version_rejected();
    return test_summary("settings_codec_locale");
}
