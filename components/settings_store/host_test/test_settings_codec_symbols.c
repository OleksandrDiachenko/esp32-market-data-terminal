#include "test_common.h"
#include "settings_codec.h"

static void test_roundtrip(void)
{
    symbol_settings_t cfg;
    settings_symbols_init_default(&cfg);
    cfg.count = 2;
    strncpy(cfg.symbols[0].ticker, "AAPL", SETTINGS_SYMBOL_MAX_LEN);
    strncpy(cfg.symbols[1].ticker, "BTCUSDT", SETTINGS_SYMBOL_MAX_LEN);

    settings_symbols_seal(&cfg);

    CHECK(settings_symbols_validate(&cfg) == SETTINGS_CODEC_OK);
    CHECK_STREQ(cfg.symbols[0].ticker, "AAPL");
    CHECK_STREQ(cfg.symbols[1].ticker, "BTCUSDT");
}

static void test_bad_crc_rejected(void)
{
    symbol_settings_t cfg;
    settings_symbols_init_default(&cfg);
    settings_symbols_seal(&cfg);
    cfg.crc32 ^= 0xFFFFFFFFu;

    CHECK(settings_symbols_validate(&cfg) == SETTINGS_CODEC_BAD_CRC);
}

static void test_bad_magic_rejected(void)
{
    symbol_settings_t cfg;
    settings_symbols_init_default(&cfg);
    settings_symbols_seal(&cfg);
    cfg.magic = 0xDEADBEEFu;

    CHECK(settings_symbols_validate(&cfg) == SETTINGS_CODEC_BAD_MAGIC);
}

static void test_bad_version_rejected(void)
{
    symbol_settings_t cfg;
    settings_symbols_init_default(&cfg);
    settings_symbols_seal(&cfg);
    cfg.version = 99;

    CHECK(settings_symbols_validate(&cfg) == SETTINGS_CODEC_BAD_VERSION);
}

static void test_count_out_of_range_rejected(void)
{
    symbol_settings_t cfg;
    settings_symbols_init_default(&cfg);
    cfg.count = SETTINGS_MAX_WATCHLIST + 1;
    settings_symbols_seal(&cfg);

    CHECK(settings_symbols_validate(&cfg) == SETTINGS_CODEC_BAD_RANGE);
}

static void test_empty_watchlist_is_valid(void)
{
    symbol_settings_t cfg;
    settings_symbols_init_default(&cfg);

    CHECK(cfg.count == 0);
    settings_symbols_seal(&cfg);

    CHECK(settings_symbols_validate(&cfg) == SETTINGS_CODEC_OK);
    CHECK(cfg.count == 0);
}

int main(void)
{
    test_roundtrip();
    test_bad_crc_rejected();
    test_bad_magic_rejected();
    test_bad_version_rejected();
    test_count_out_of_range_rejected();
    test_empty_watchlist_is_valid();
    return test_summary("settings_codec_symbols");
}
