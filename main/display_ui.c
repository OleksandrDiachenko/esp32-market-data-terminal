#include "display_ui.h"

#include "app_state.h"
#include "board_jc4880p443c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"
#include "settings_store.h"
#include "time_sync.h"
#include "wifi_manager.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "display_ui";

#define ROW_HEIGHT_PX 76
#define ROW_SIDE_COL_WIDTH_PX 128
#define STATUSBAR_HEIGHT_PX 40
#define SETTINGS_ROW_HEIGHT_PX 64
#define SUBHEADER_HEIGHT_PX 56
#define UPDATE_PERIOD_MS 1000

#define COLOR_INK lv_color_hex(0x0A0C10)
#define COLOR_STATUSBAR_BG lv_color_hex(0x0F1216)
#define COLOR_TEXT lv_color_hex(0xECEEF2)
#define COLOR_MUTED lv_color_hex(0x6E7686)
#define COLOR_UP lv_color_hex(0x2FD481)
#define COLOR_DOWN lv_color_hex(0xFF5D6C)
#define COLOR_WARN lv_color_hex(0xF2A93C)
#define COLOR_HAIRLINE lv_color_hex(0x1B1F26)
#define COLOR_ACCENT lv_color_hex(0x47C9FF)

typedef enum
{
    DISPLAY_UI_SCREEN_WATCHLIST = 0,
    DISPLAY_UI_SCREEN_SETTINGS,
} display_ui_screen_t;

// Which Settings sub-screen is showing. More views (Wi-Fi, watchlist
// management, updates) land in later Phase 11 slices.
typedef enum
{
    SETTINGS_VIEW_LIST = 0,
    SETTINGS_VIEW_LOCALE,
} settings_view_t;

// One LVGL row per watchlist symbol. Built once per (re)load of the
// watchlist (see rebuild_rows_if_needed()); every subsequent timer tick only
// updates these existing objects' content - no per-tick create/delete.
typedef struct
{
    lv_obj_t *row;
    lv_obj_t *ticker_label;
    lv_obj_t *range_label;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    lv_obj_t *price_label;
    lv_obj_t *change_label;
} display_ui_row_t;

static lv_obj_t *s_rows_container;
static display_ui_row_t s_rows[APP_STATE_MAX_SYMBOLS];
static uint8_t s_row_count;
static lv_timer_t *s_update_timer;

static lv_obj_t *s_settings_list;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_conn_label;
static lv_obj_t *s_nav_label;
static display_ui_screen_t s_active_screen;
static settings_view_t s_settings_view;

static lv_obj_t *s_locale_screen;
static lv_obj_t *s_locale_tz_input;
static lv_obj_t *s_locale_keyboard;

// Loaded once at startup; kept up to date by locale_24h_toggle_cb()/
// locale_tz_ready_cb() as the Locale screen saves changes.
static locale_settings_t s_locale;

// Reused scratch buffers: avoids per-tick dynamic allocation (AGENTS.md: no
// dynamic allocation in the hot path) and avoids putting
// sizeof(market_data_kline_t) * APP_STATE_KLINE_CAPACITY (~25KB) on the LVGL
// port task's stack. Safe because update_row() runs rows strictly one at a
// time within a single timer callback - no concurrent use.
static market_data_kline_t s_klines_scratch[APP_STATE_KLINE_CAPACITY];
static int32_t s_chart_scratch[APP_STATE_MAX_SYMBOLS][APP_STATE_KLINE_CAPACITY];

static void format_price(double value, char *out, size_t out_len)
{
    // Sub-$1 pairs (e.g. ADAUSDT, DOGEUSDT) need more decimals to show any
    // meaningful movement; higher-value pairs would be noisy at 4 decimals.
    if (value >= 1.0)
    {
        snprintf(out, out_len, "%.2f", value);
    }
    else
    {
        snprintf(out, out_len, "%.4f", value);
    }
}

static void destroy_rows(void)
{
    for (uint8_t i = 0; i < s_row_count; i++)
    {
        lv_obj_delete(s_rows[i].row);
    }
    s_row_count = 0;
}

static void build_row(uint8_t index)
{
    display_ui_row_t *row = &s_rows[index];

    row->row = lv_obj_create(s_rows_container);
    lv_obj_remove_style_all(row->row);
    lv_obj_set_size(row->row, LV_PCT(100), ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(row->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row->row, 18, 0);
    lv_obj_set_style_pad_right(row->row, 18, 0);
    lv_obj_set_style_pad_column(row->row, 14, 0);
    lv_obj_set_style_border_side(row->row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row->row, 1, 0);
    lv_obj_set_style_border_color(row->row, COLOR_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(row->row, LV_OPA_TRANSP, 0);

    // Left: ticker + 24h range.
    lv_obj_t *left = lv_obj_create(row->row);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, ROW_SIDE_COL_WIDTH_PX, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 4, 0);

    row->ticker_label = lv_label_create(left);
    lv_obj_set_style_text_color(row->ticker_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(row->ticker_label, &lv_font_montserrat_16, 0);

    row->range_label = lv_label_create(left);
    lv_obj_set_style_text_color(row->range_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(row->range_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(row->range_label, "-- / --");

    // Middle: sparkline.
    row->chart = lv_chart_create(row->row);
    lv_obj_set_flex_grow(row->chart, 1);
    lv_obj_set_height(row->chart, 40);
    lv_chart_set_type(row->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(row->chart, 0, 0);
    lv_obj_set_style_bg_opa(row->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row->chart, 0, 0);
    lv_obj_set_style_size(row->chart, 0, 0, LV_PART_INDICATOR); // hide point markers
    lv_obj_set_style_line_width(row->chart, 2, LV_PART_ITEMS);
    row->series = lv_chart_add_series(row->chart, COLOR_MUTED, LV_CHART_AXIS_PRIMARY_Y);

    // Right: price + change, right-aligned.
    lv_obj_t *right = lv_obj_create(row->row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, ROW_SIDE_COL_WIDTH_PX, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(right, 4, 0);

    row->price_label = lv_label_create(right);
    lv_obj_set_style_text_color(row->price_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(row->price_label, &lv_font_montserrat_18, 0);

    row->change_label = lv_label_create(right);
    lv_obj_set_style_text_color(row->change_label, COLOR_MUTED, 0);
    lv_label_set_text(row->change_label, "Loading...");
}

static void rebuild_rows_if_needed(uint8_t count)
{
    if (count == s_row_count)
    {
        return;
    }

    destroy_rows();
    for (uint8_t i = 0; i < count; i++)
    {
        build_row(i);
    }
    s_row_count = count;
}

static void update_row(uint8_t index)
{
    display_ui_row_t *row = &s_rows[index];

    app_state_symbol_meta_t meta;
    if (app_state_get_symbol_meta(index, &meta) != ESP_OK)
    {
        return;
    }

    lv_label_set_text(row->ticker_label, meta.symbol);

    if (meta.state == APP_STATE_SYMBOL_ERROR)
    {
        lv_label_set_text(row->price_label, "");
        lv_obj_set_style_text_color(row->change_label, COLOR_WARN, 0);
        lv_label_set_text(row->change_label, "Unavailable");
        return; // keep the last-known range/chart, if any, for context
    }

    if (meta.kline_count == 0)
    {
        // Never synced yet (INIT), or DEGRADED before a first sync ever
        // succeeded - nothing to compute a price/range/chart from.
        lv_label_set_text(row->range_label, "-- / --");
        lv_label_set_text(row->price_label, "");
        lv_obj_set_style_text_color(row->change_label, COLOR_MUTED, 0);
        lv_label_set_text(row->change_label, meta.state == APP_STATE_SYMBOL_DEGRADED ? "Resyncing..." : "Loading...");
        return;
    }

    uint16_t count = 0;
    esp_err_t err = app_state_get_symbol_klines(index, s_klines_scratch, APP_STATE_KLINE_CAPACITY, &count);
    if (err != ESP_OK || count == 0)
    {
        return; // keep showing the previous tick's values
    }

    double first_open = s_klines_scratch[0].open;
    double last_close = s_klines_scratch[count - 1].close;
    double hi = s_klines_scratch[0].high;
    double lo = s_klines_scratch[0].low;
    for (uint16_t i = 1; i < count; i++)
    {
        if (s_klines_scratch[i].high > hi)
        {
            hi = s_klines_scratch[i].high;
        }
        if (s_klines_scratch[i].low < lo)
        {
            lo = s_klines_scratch[i].low;
        }
    }

    bool up = last_close >= first_open;
    double change_pct = (first_open != 0.0) ? ((last_close - first_open) / first_open) * 100.0 : 0.0;

    char price_buf[24];
    char hi_buf[24];
    char lo_buf[24];
    char range_buf[2 * sizeof(hi_buf) + 4]; // "<hi> / <lo>" plus separator and NUL
    char change_buf[16];
    format_price(last_close, price_buf, sizeof(price_buf));
    format_price(hi, hi_buf, sizeof(hi_buf));
    format_price(lo, lo_buf, sizeof(lo_buf));
    snprintf(range_buf, sizeof(range_buf), "%s / %s", hi_buf, lo_buf);
    snprintf(change_buf, sizeof(change_buf), "%+.2f%%", change_pct);

    lv_label_set_text(row->range_label, range_buf);
    lv_chart_set_series_color(row->chart, row->series, up ? COLOR_UP : COLOR_DOWN);

    int32_t min_scaled = (int32_t)lround(lo * 100.0);
    int32_t max_scaled = (int32_t)lround(hi * 100.0);
    if (min_scaled == max_scaled)
    {
        max_scaled = min_scaled + 1; // avoid a zero-height axis range
    }
    lv_chart_set_axis_range(row->chart, LV_CHART_AXIS_PRIMARY_Y, min_scaled, max_scaled);

    int32_t *chart_data = s_chart_scratch[index];
    for (uint16_t i = 0; i < count; i++)
    {
        chart_data[i] = (int32_t)lround(s_klines_scratch[i].close * 100.0);
    }
    lv_chart_set_point_count(row->chart, count);
    lv_chart_set_series_ext_y_array(row->chart, row->series, chart_data);
    lv_chart_refresh(row->chart);

    if (meta.state == APP_STATE_SYMBOL_DEGRADED)
    {
        // Range/chart above still reflect the last successful sync; only
        // the quote itself is replaced, matching the reviewed design.
        lv_label_set_text(row->price_label, "");
        lv_obj_set_style_text_color(row->change_label, COLOR_WARN, 0);
        lv_label_set_text(row->change_label, "Resyncing...");
    }
    else
    {
        lv_label_set_text(row->price_label, price_buf);
        lv_obj_set_style_text_color(row->change_label, up ? COLOR_UP : COLOR_DOWN, 0);
        lv_label_set_text(row->change_label, change_buf);
    }
}

// Shows exactly one of the Settings sub-screens, hiding the rest. Only
// meaningful while DISPLAY_UI_SCREEN_SETTINGS is active.
static void show_settings_view(settings_view_t view)
{
    s_settings_view = view;
    lv_obj_add_flag(s_settings_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_locale_screen, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *target = (view == SETTINGS_VIEW_LOCALE) ? s_locale_screen : s_settings_list;
    lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
}

static void set_active_screen(display_ui_screen_t screen)
{
    s_active_screen = screen;

    if (screen == DISPLAY_UI_SCREEN_WATCHLIST)
    {
        lv_obj_remove_flag(s_rows_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_settings_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_locale_screen, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_nav_label, LV_SYMBOL_SETTINGS " Settings");
    }
    else
    {
        lv_obj_add_flag(s_rows_container, LV_OBJ_FLAG_HIDDEN);
        show_settings_view(SETTINGS_VIEW_LIST); // always re-enter Settings at its top level
        lv_label_set_text(s_nav_label, LV_SYMBOL_LEFT " Exit");
    }
}

// One button, context-dependent: "Settings" while on the watchlist, "Exit"
// (back to the watchlist, from anywhere in Settings) while on Settings -
// not two persistent tabs. Each Settings sub-screen has its own back
// control for stepping back one level instead.
static void nav_click_cb(lv_event_t *e)
{
    (void)e;
    set_active_screen(s_active_screen == DISPLAY_UI_SCREEN_WATCHLIST ? DISPLAY_UI_SCREEN_SETTINGS
                                                                      : DISPLAY_UI_SCREEN_WATCHLIST);
}

static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LIST);
}

static void update_statusbar(void)
{
    if (time_sync_is_synced())
    {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char clock_buf[24];
        strftime(clock_buf, sizeof(clock_buf), s_locale.time_24h ? "%d %b %Y  %H:%M" : "%d %b %Y  %I:%M %p", &tm_now);
        lv_label_set_text(s_clock_label, clock_buf);
    }
    else
    {
        lv_label_set_text(s_clock_label, "--:--");
    }

    wifi_manager_snapshot_t snapshot;
    if (wifi_manager_get_snapshot(&snapshot) != ESP_OK)
    {
        return;
    }

    switch (snapshot.state)
    {
    case WIFI_MANAGER_STATE_CONNECTED:
        lv_obj_set_style_text_color(s_conn_label, COLOR_UP, 0);
        lv_label_set_text(s_conn_label, LV_SYMBOL_WIFI " Connected");
        break;
    case WIFI_MANAGER_STATE_CONNECTING:
        lv_obj_set_style_text_color(s_conn_label, COLOR_WARN, 0);
        lv_label_set_text(s_conn_label, LV_SYMBOL_WIFI " Connecting...");
        break;
    case WIFI_MANAGER_STATE_ERROR:
        lv_obj_set_style_text_color(s_conn_label, COLOR_WARN, 0);
        lv_label_set_text(s_conn_label, LV_SYMBOL_WIFI " Reconnecting...");
        break;
    default:
        lv_obj_set_style_text_color(s_conn_label, COLOR_MUTED, 0);
        lv_label_set_text(s_conn_label, LV_SYMBOL_WIFI " Offline");
        break;
    }
}

static void update_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint8_t count = app_state_symbol_count();
    if (count > APP_STATE_MAX_SYMBOLS)
    {
        count = APP_STATE_MAX_SYMBOLS; // defensive; settings_store already bounds this
    }

    rebuild_rows_if_needed(count);

    for (uint8_t i = 0; i < s_row_count; i++)
    {
        update_row(i);
    }

    update_statusbar();
}

static void build_statusbar(lv_obj_t *screen)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    // Base lv_obj_create() defaults add scroll/gesture/focus flags meant
    // for interactive, scrollable containers - none apply to a plain status
    // bar, and left enabled they can make a tap on a child register as an
    // aborted scroll/gesture instead of a click.
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN |
                                LV_OBJ_FLAG_SCROLL_WITH_ARROW | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_PRESS_LOCK |
                                LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(bar, LV_PCT(100), STATUSBAR_HEIGHT_PX);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, 16, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COLOR_HAIRLINE, 0);
    lv_obj_set_style_bg_color(bar, COLOR_STATUSBAR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    s_clock_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xB7BCC5), 0);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_clock_label, "--:--");

    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE |
                                   LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                   LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_WITH_ARROW |
                                   LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_GESTURE_BUBBLE);
    // Width defaults to LV_DPI_DEF (a fixed ~130px), not content-fitting -
    // left unset, "Connected"/"Exit" (wider than the initial "--"
    // placeholder) would overflow past that fixed box and clip on the left
    // since this container is packed flush to the bar's right edge.
    lv_obj_set_width(right, LV_SIZE_CONTENT);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 16, 0);

    s_conn_label = lv_label_create(right);
    lv_obj_set_style_text_font(s_conn_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_conn_label, LV_SYMBOL_WIFI " --");

    lv_obj_t *nav_btn = lv_button_create(right);
    lv_obj_remove_style_all(nav_btn);
    // lv_button_create() still leaves several scroll/gesture/focus flags on
    // from the base object (only SCROLLABLE itself is cleared) - none of
    // them apply to a single-purpose nav button, and combined with the
    // ancestors' own default flags they could make a tap register as an
    // aborted scroll/gesture instead of a click. Only CLICKABLE is needed.
    lv_obj_remove_flag(nav_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                                    LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN |
                                    LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_SCROLL_WITH_ARROW |
                                    LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_GESTURE_BUBBLE);
    // lv_obj_remove_style_all() also resets width/height back to the base
    // class default (a fixed ~130x130px, LV_DPI_DEF) - left unset, the
    // button was far taller than the 40px bar and got clipped by it. Fill
    // the bar's full height instead, and let width fit the padded label;
    // remove_style_all also dropped the button's own default centering
    // layout, so that's reapplied explicitly too.
    lv_obj_set_height(nav_btn, STATUSBAR_HEIGHT_PX);
    lv_obj_set_width(nav_btn, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nav_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // A label-sized hit target is as hard to tap as a hyperlink - pad the
    // button past its text, then extend the touch-sensitive area further
    // still, without changing how big it looks.
    lv_obj_set_style_pad_top(nav_btn, 4, 0);
    lv_obj_set_style_pad_bottom(nav_btn, 4, 0);
    lv_obj_set_style_pad_left(nav_btn, 10, 0);
    lv_obj_set_style_pad_right(nav_btn, 10, 0);
    lv_obj_set_ext_click_area(nav_btn, 16);
    lv_obj_add_event_cb(nav_btn, nav_click_cb, LV_EVENT_CLICKED, NULL);
    s_nav_label = lv_label_create(nav_btn);
    lv_obj_set_style_text_color(s_nav_label, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_nav_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_nav_label, LV_SYMBOL_SETTINGS " Settings");
}

// Strips the same interactive-container defaults from a plain lv_obj_create()
// as build_statusbar()'s bar/right containers - see the comment there. Kept
// as a helper here since Settings adds several more such containers.
static void make_plain_container(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN |
                                LV_OBJ_FLAG_SCROLL_WITH_ARROW | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_PRESS_LOCK |
                                LV_OBJ_FLAG_GESTURE_BUBBLE);
}

// A tappable row: title on the left, a chevron on the right. Caller attaches
// its own click handler; more Settings rows (Wi-Fi, watchlist, updates) land
// in later slices.
static lv_obj_t *build_settings_row(lv_obj_t *parent, const char *title, lv_event_cb_t click_cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), SETTINGS_ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title_label = lv_label_create(row);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(title_label, title);

    lv_obj_t *chevron = lv_label_create(row);
    lv_obj_set_style_text_color(chevron, COLOR_MUTED, 0);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);

    return row;
}

// A sub-screen header: back arrow (calling back_cb) + title, matching the
// reviewed design. Reused by every Settings sub-screen.
static void build_subscreen_header(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    make_plain_container(header);
    lv_obj_set_size(header, LV_PCT(100), SUBHEADER_HEIGHT_PX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_obj_set_style_pad_column(header, 10, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, COLOR_HAIRLINE, 0);

    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_remove_style_all(back_btn);
    make_plain_container(back_btn);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(back_btn, 40, 40);
    lv_obj_set_flex_flow(back_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(back_btn, 10);
    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_16, 0);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);

    lv_obj_t *title_label = lv_label_create(header);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(title_label, title);
}

static void wifi_row_click_cb(lv_event_t *e)
{
    (void)e;
    // Wi-Fi connectivity UI lands in the next slice.
}

static void locale_row_click_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LOCALE);
}

static void build_settings_list(lv_obj_t *screen)
{
    s_settings_list = lv_obj_create(screen);
    lv_obj_remove_style_all(s_settings_list);
    make_plain_container(s_settings_list);
    lv_obj_set_size(s_settings_list, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_settings_list, LV_FLEX_FLOW_COLUMN);

    build_settings_row(s_settings_list, LV_SYMBOL_WIFI " Wi-Fi", wifi_row_click_cb);
    build_settings_row(s_settings_list, LV_SYMBOL_SETTINGS " Date & time", locale_row_click_cb);
}

static void locale_24h_toggle_cb(lv_event_t *e)
{
    lv_obj_t *toggle = lv_event_get_target(e);
    s_locale.time_24h = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    settings_store_save_locale(&s_locale);
}

static void locale_tz_focus_cb(lv_event_t *e)
{
    (void)e;
    lv_keyboard_set_textarea(s_locale_keyboard, s_locale_tz_input);
    lv_obj_remove_flag(s_locale_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// Fires when the keyboard's own Enter/checkmark key is tapped - saves and
// applies the new zone immediately (mirrors time_sync_start()'s own
// setenv()+tzset(), so the clock updates without a reboot).
static void locale_tz_ready_cb(lv_event_t *e)
{
    (void)e;
    const char *text = lv_textarea_get_text(s_locale_tz_input);
    strncpy(s_locale.posix_tz, text, SETTINGS_POSIX_TZ_MAX_LEN);
    s_locale.posix_tz[SETTINGS_POSIX_TZ_MAX_LEN] = '\0';
    settings_store_save_locale(&s_locale);
    setenv("TZ", s_locale.posix_tz, 1);
    tzset();
    lv_obj_add_flag(s_locale_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void build_locale_screen(lv_obj_t *screen)
{
    s_locale_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_locale_screen);
    make_plain_container(s_locale_screen);
    lv_obj_set_size(s_locale_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_locale_screen, LV_FLEX_FLOW_COLUMN);

    build_subscreen_header(s_locale_screen, "Date & time", settings_back_cb);

    lv_obj_t *toggle_row = lv_obj_create(s_locale_screen);
    lv_obj_remove_style_all(toggle_row);
    make_plain_container(toggle_row);
    lv_obj_set_size(toggle_row, LV_PCT(100), SETTINGS_ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(toggle_row, 20, 0);
    lv_obj_set_style_pad_right(toggle_row, 20, 0);
    lv_obj_set_style_border_side(toggle_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(toggle_row, 1, 0);
    lv_obj_set_style_border_color(toggle_row, COLOR_HAIRLINE, 0);

    lv_obj_t *toggle_label = lv_label_create(toggle_row);
    lv_obj_set_style_text_color(toggle_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(toggle_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(toggle_label, "24-hour clock");

    lv_obj_t *toggle = lv_switch_create(toggle_row);
    if (s_locale.time_24h)
    {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(toggle, locale_24h_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tz_label = lv_label_create(s_locale_screen);
    lv_obj_set_style_text_color(tz_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(tz_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_top(tz_label, 14, 0);
    lv_obj_set_style_pad_left(tz_label, 20, 0);
    lv_label_set_text(tz_label, "TIME ZONE (POSIX TZ STRING)");

    s_locale_tz_input = lv_textarea_create(s_locale_screen);
    lv_textarea_set_one_line(s_locale_tz_input, true);
    lv_textarea_set_max_length(s_locale_tz_input, SETTINGS_POSIX_TZ_MAX_LEN);
    lv_textarea_set_placeholder_text(s_locale_tz_input, "e.g. UTC0 or EST5EDT,M3.2.0,M11.1.0");
    lv_obj_set_size(s_locale_tz_input, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(s_locale_tz_input, 20, 0);
    lv_obj_set_style_margin_top(s_locale_tz_input, 6, 0);
    lv_obj_add_event_cb(s_locale_tz_input, locale_tz_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_locale_tz_input, locale_tz_ready_cb, LV_EVENT_READY, NULL);

    s_locale_keyboard = lv_keyboard_create(s_locale_screen);
    lv_keyboard_set_mode(s_locale_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_flag(s_locale_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void display_ui_render(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COLOR_INK, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, COLOR_TEXT, 0);

    s_rows_container = lv_obj_create(screen);
    lv_obj_remove_style_all(s_rows_container);
    lv_obj_set_size(s_rows_container, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_rows_container, LV_FLEX_FLOW_COLUMN);

    settings_store_load_locale(&s_locale); // build_locale_screen() below needs it for the 24h switch's initial state

    build_settings_list(screen);
    build_locale_screen(screen);
    build_statusbar(screen);

    set_active_screen(DISPLAY_UI_SCREEN_WATCHLIST);

    s_row_count = 0;
    // app_state_init() (and thus a non-zero app_state_symbol_count()) may
    // not have run yet when this is called - rebuild_rows_if_needed() picks
    // the watchlist up on whichever tick first sees it loaded.
    s_update_timer = lv_timer_create(update_timer_cb, UPDATE_PERIOD_MS, NULL);
}

esp_err_t display_ui_start(void)
{
    lv_display_t *display = NULL;
    ESP_RETURN_ON_ERROR(board_jc4880p443c_display_start(&display), TAG, "start display");

    lv_indev_t *touch_indev = NULL;
    ESP_RETURN_ON_ERROR(board_jc4880p443c_touch_start(display, &touch_indev), TAG, "start touch");

    ESP_RETURN_ON_ERROR(board_jc4880p443c_backlight_on(), TAG, "turn on backlight");

    if (!board_jc4880p443c_display_lock(0))
    {
        ESP_LOGE(TAG, "failed to acquire LVGL lock");
        return ESP_FAIL;
    }
    display_ui_render();
    board_jc4880p443c_display_unlock();

    ESP_LOGI(TAG, "Display UI started.");
    return ESP_OK;
}
