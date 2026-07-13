#include "display_ui_logic.h"
#include "test_common.h"

static void test_relative_time(void)
{
    char out[32];
    display_ui_logic_format_relative_time(0, 100000, out, sizeof(out));
    CHECK_STREQ(out, "Never");
    display_ui_logic_format_relative_time(100000, 159999, out, sizeof(out));
    CHECK_STREQ(out, "Just now");
    display_ui_logic_format_relative_time(100000, 160000, out, sizeof(out));
    CHECK_STREQ(out, "1 min ago");
    display_ui_logic_format_relative_time(100000, 3700000, out, sizeof(out));
    CHECK_STREQ(out, "1 h ago");
    display_ui_logic_format_relative_time(100000, 86500000, out, sizeof(out));
    CHECK_STREQ(out, "1 d ago");
    display_ui_logic_format_relative_time(200000, 100000, out, sizeof(out));
    CHECK_STREQ(out, "Just now");
}

static void test_rssi_bars(void)
{
    CHECK(display_ui_logic_rssi_to_bars(-30) == 4);
    CHECK(display_ui_logic_rssi_to_bars(-55) == 4);
    CHECK(display_ui_logic_rssi_to_bars(-56) == 3);
    CHECK(display_ui_logic_rssi_to_bars(-67) == 3);
    CHECK(display_ui_logic_rssi_to_bars(-68) == 2);
    CHECK(display_ui_logic_rssi_to_bars(-78) == 2);
    CHECK(display_ui_logic_rssi_to_bars(-79) == 1);
    CHECK(display_ui_logic_rssi_to_bars(-100) == 1);
}

static void test_symbol_sanitizing(void)
{
    char out[16];
    CHECK(display_ui_logic_sanitize_symbol("ltc usdt!", out, sizeof(out)) == 7);
    CHECK_STREQ(out, "LTCUSDT");
    CHECK(display_ui_logic_sanitize_symbol("-_.", out, sizeof(out)) == 0);
    CHECK_STREQ(out, "");
    CHECK(display_ui_logic_sanitize_symbol("abcdef", out, 4) == 3);
    CHECK_STREQ(out, "ABC");
    CHECK(display_ui_logic_sanitize_symbol(NULL, out, sizeof(out)) == 0);
    CHECK(display_ui_logic_sanitize_symbol("BTCUSDT", NULL, 0) == 0);
}

static void test_night_window(void)
{
    CHECK(display_ui_logic_minute_in_window(22U * 60U, 22U * 60U, 7U * 60U));
    CHECK(display_ui_logic_minute_in_window(6U * 60U + 59U, 22U * 60U, 7U * 60U));
    CHECK(!display_ui_logic_minute_in_window(7U * 60U, 22U * 60U, 7U * 60U));
    CHECK(!display_ui_logic_minute_in_window(12U * 60U, 22U * 60U, 7U * 60U));
    CHECK(display_ui_logic_minute_in_window(10U * 60U, 10U * 60U, 18U * 60U));
    CHECK(!display_ui_logic_minute_in_window(18U * 60U, 10U * 60U, 18U * 60U));
    CHECK(!display_ui_logic_minute_in_window(10U * 60U, 10U * 60U, 10U * 60U));
    CHECK(!display_ui_logic_minute_in_window(1440U, 0U, 60U));
}

int main(void)
{
    test_relative_time();
    test_rssi_bars();
    test_symbol_sanitizing();
    test_night_window();
    printf("[display_ui_logic] %d/%d checks passed\n", g_checks, g_checks);
    return 0;
}
