#include "test_common.h"
#include "settings_codec.h"

static void test_default_is_never_acknowledged(void)
{
    disclaimer_settings_t cfg;
    settings_disclaimer_init_default(&cfg);
    settings_disclaimer_seal(&cfg);

    CHECK(settings_disclaimer_validate(&cfg) == SETTINGS_CODEC_OK);
    CHECK(cfg.acked_fw_version[0] == '\0');
}

static void test_roundtrip(void)
{
    disclaimer_settings_t cfg;
    settings_disclaimer_init_default(&cfg);
    strncpy(cfg.acked_fw_version, "1.2.3", SETTINGS_ACKED_FW_VERSION_MAX_LEN);

    settings_disclaimer_seal(&cfg);

    CHECK(settings_disclaimer_validate(&cfg) == SETTINGS_CODEC_OK);
    CHECK(strcmp(cfg.acked_fw_version, "1.2.3") == 0);
}

static void test_bad_crc_rejected(void)
{
    disclaimer_settings_t cfg;
    settings_disclaimer_init_default(&cfg);
    settings_disclaimer_seal(&cfg);
    cfg.crc32 ^= 0xFFFFFFFFu;

    CHECK(settings_disclaimer_validate(&cfg) == SETTINGS_CODEC_BAD_CRC);
}

static void test_bad_magic_rejected(void)
{
    disclaimer_settings_t cfg;
    settings_disclaimer_init_default(&cfg);
    settings_disclaimer_seal(&cfg);
    cfg.magic = 0xDEADBEEFu;

    CHECK(settings_disclaimer_validate(&cfg) == SETTINGS_CODEC_BAD_MAGIC);
}

static void test_bad_version_rejected(void)
{
    disclaimer_settings_t cfg;
    settings_disclaimer_init_default(&cfg);
    settings_disclaimer_seal(&cfg);
    cfg.version = 99;

    CHECK(settings_disclaimer_validate(&cfg) == SETTINGS_CODEC_BAD_VERSION);
}

static void test_should_show_true_when_never_acknowledged(void)
{
    CHECK(settings_disclaimer_should_show("", "1.0.0") == true);
    CHECK(settings_disclaimer_should_show(NULL, "1.0.0") == true);
}

static void test_should_show_false_once_acknowledged_for_running_version(void)
{
    CHECK(settings_disclaimer_should_show("1.0.0", "1.0.0") == false);
}

static void test_should_show_true_when_version_changed(void)
{
    // Covers both an OTA update (acked stays behind the new running
    // version) and a downgrade/rollback - any mismatch re-shows it.
    CHECK(settings_disclaimer_should_show("1.0.0", "1.0.1") == true);
    CHECK(settings_disclaimer_should_show("1.0.1", "1.0.0") == true);
}

static void test_should_show_false_when_running_is_empty(void)
{
    // Defensive: an empty running version string can't be compared
    // meaningfully, so never gate boot on it.
    CHECK(settings_disclaimer_should_show("1.0.0", "") == false);
    CHECK(settings_disclaimer_should_show("1.0.0", NULL) == false);
}

int main(void)
{
    test_default_is_never_acknowledged();
    test_roundtrip();
    test_bad_crc_rejected();
    test_bad_magic_rejected();
    test_bad_version_rejected();
    test_should_show_true_when_never_acknowledged();
    test_should_show_false_once_acknowledged_for_running_version();
    test_should_show_true_when_version_changed();
    test_should_show_false_when_running_is_empty();
    return test_summary("settings_codec_disclaimer");
}
