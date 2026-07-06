#include "display_ui.h"

#include "app_state.h"
#include "board_jc4880p443c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "settings_store.h"
#include "time_sync.h"
#include "wifi_manager.h"

#if CONFIG_DEV_SCREENSHOT_CONSOLE
#include "esp_console.h"
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "display_ui";

#define ROW_HEIGHT_PX 76
#define ROW_SIDE_COL_WIDTH_PX 128
#define STATUSBAR_HEIGHT_PX 40
#define SETTINGS_ROW_HEIGHT_PX 108
#define SETTINGS_LIST_HEADER_HEIGHT_PX 64
#define SUBHEADER_HEIGHT_PX 64
#define SETTINGS_ICON_CHIP_PX 40
#define NAV_BUTTON_WIDTH_PX 130
#define WIFI_PASSWORD_FIELD_HEIGHT_PX 54 // matches the eye-toggle button below it
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
    SETTINGS_VIEW_WIFI,
    SETTINGS_VIEW_WIFI_PASSWORD,
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
static lv_obj_t *s_settings_wifi_row_desc;
static lv_obj_t *s_settings_locale_row_desc;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_conn_label;
static lv_obj_t *s_nav_label;
static display_ui_screen_t s_active_screen;
static settings_view_t s_settings_view;

static lv_obj_t *s_locale_screen;

// Loaded once at startup; kept up to date by locale_24h_toggle_cb() as the
// Locale screen saves changes.
static locale_settings_t s_locale;

static lv_obj_t *s_wifi_screen;
static lv_obj_t *s_wifi_list;
// Static (not stack-local) so completed rows' click handlers can safely
// reference "their" wifi_manager_ap_t by pointer after the function that
// built them returns - see build_wifi_ap_row()/wifi_ap_click_cb().
static wifi_manager_snapshot_t s_wifi_snapshot;

static lv_obj_t *s_wifi_password_screen;
static lv_obj_t *s_wifi_ssid_input;
static lv_obj_t *s_wifi_password_input;
static lv_obj_t *s_wifi_password_keyboard;
static lv_obj_t *s_wifi_password_status;
static char s_wifi_pending_ssid[WIFI_MANAGER_SSID_MAX + 1];
// true once the SSID field is showing a network picked from the scan list
// (read-only there); false for the "Add Network" flow, where the SSID field
// is itself editable via the shared keyboard.
static bool s_wifi_ssid_known;
// Set on Connect tap, cleared once wifi_password_poll() (called from the
// main 1s update timer) sees the attempt resolve one way or another - see
// wifi_manager_snapshot_t's last_event/last_event_seq in wifi_manager.h.
static bool s_wifi_connecting;
static uint32_t s_wifi_connect_baseline_seq;
static uint8_t s_wifi_connect_ticks;
#define WIFI_CONNECT_TIMEOUT_TICKS 20 // ~20s at the 1s update-timer cadence

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

// Fills the area under a row's sparkline with a fade from the series color
// (at the line) to fully transparent (at the chart's bottom edge).
// lv_chart has no built-in area-fill for LV_CHART_TYPE_LINE, so this hooks
// LV_EVENT_DRAW_TASK_ADDED and replicates the technique from the vendored
// managed_components/lvgl__lvgl/demos/widgets/lv_demo_widgets_analytics.c
// (its own dashboard area chart), simplified here for a single series per
// chart - no need for that demo's multi-series id lookup.
static void chart_draw_event_cb(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_target(e);
    display_ui_row_t *row = (display_ui_row_t *)lv_event_get_user_data(e);
    lv_draw_task_t *draw_task = lv_event_get_param(e);
    lv_draw_dsc_base_t *base_dsc = lv_draw_task_get_draw_dsc(draw_task);
    lv_draw_line_dsc_t *draw_line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (base_dsc->part != LV_PART_ITEMS || draw_line_dsc == NULL)
    {
        return;
    }

    lv_area_t obj_coords;
    lv_obj_get_coords(chart, &obj_coords);
    lv_color_t color = lv_chart_get_series_color(chart, row->series);
    int32_t full_h = lv_obj_get_height(chart);

    for (int32_t i = 0; i < draw_line_dsc->point_cnt - 1; i++)
    {
        lv_point_precise_t p1 = draw_line_dsc->points[i];
        lv_point_precise_t p2 = draw_line_dsc->points[i + 1];

        lv_draw_triangle_dsc_t tri_dsc;
        lv_draw_triangle_dsc_init(&tri_dsc);
        tri_dsc.p[0].x = (int32_t)p1.x;
        tri_dsc.p[0].y = (int32_t)p1.y;
        tri_dsc.p[1].x = (int32_t)p2.x;
        tri_dsc.p[1].y = (int32_t)p2.y;
        tri_dsc.p[2].x = (int32_t)(p1.y < p2.y ? p1.x : p2.x);
        tri_dsc.p[2].y = (int32_t)LV_MAX(p1.y, p2.y);
        tri_dsc.grad.dir = LV_GRAD_DIR_VER;

        int32_t fract_upper = (int32_t)(LV_MIN(p1.y, p2.y) - obj_coords.y1) * 255 / full_h;
        int32_t fract_lower = (int32_t)(LV_MAX(p1.y, p2.y) - obj_coords.y1) * 255 / full_h;
        tri_dsc.grad.stops[0].color = color;
        tri_dsc.grad.stops[0].opa = (lv_opa_t)(255 - fract_upper);
        tri_dsc.grad.stops[0].frac = 0;
        tri_dsc.grad.stops[1].color = color;
        tri_dsc.grad.stops[1].opa = (lv_opa_t)(255 - fract_lower);
        tri_dsc.grad.stops[1].frac = 255;
        lv_draw_triangle(base_dsc->layer, &tri_dsc);

        lv_draw_rect_dsc_t rect_dsc;
        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
        rect_dsc.bg_grad.stops[0].color = color;
        rect_dsc.bg_grad.stops[0].frac = 0;
        rect_dsc.bg_grad.stops[0].opa = (lv_opa_t)(255 - fract_lower);
        rect_dsc.bg_grad.stops[1].color = color;
        rect_dsc.bg_grad.stops[1].frac = 255;
        rect_dsc.bg_grad.stops[1].opa = 0;

        lv_area_t rect_area;
        rect_area.x1 = (int32_t)p1.x;
        rect_area.x2 = (int32_t)p2.x;
        rect_area.y1 = (int32_t)LV_MAX(p1.y, p2.y);
        rect_area.y2 = obj_coords.y2;
        lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
    }
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
    lv_obj_set_style_text_font(row->ticker_label, &lv_font_montserrat_18, 0);

    row->range_label = lv_label_create(left);
    lv_obj_set_style_text_color(row->range_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(row->range_label, &lv_font_montserrat_14, 0);
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
    lv_obj_set_style_line_width(row->chart, 3, LV_PART_ITEMS);
    row->series = lv_chart_add_series(row->chart, COLOR_MUTED, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(row->chart, chart_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, row);

    // Right: price + change, right-aligned.
    lv_obj_t *right = lv_obj_create(row->row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, ROW_SIDE_COL_WIDTH_PX, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(right, 4, 0);

    row->price_label = lv_label_create(right);
    lv_obj_set_style_text_color(row->price_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(row->price_label, &lv_font_montserrat_20, 0);

    row->change_label = lv_label_create(right);
    lv_obj_set_style_text_color(row->change_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(row->change_label, &lv_font_montserrat_16, 0);
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
    lv_obj_add_flag(s_wifi_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_password_screen, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *target = s_settings_list;
    switch (view)
    {
    case SETTINGS_VIEW_LOCALE:
        target = s_locale_screen;
        break;
    case SETTINGS_VIEW_WIFI:
        target = s_wifi_screen;
        break;
    case SETTINGS_VIEW_WIFI_PASSWORD:
        target = s_wifi_password_screen;
        break;
    default:
        break;
    }
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
        lv_obj_add_flag(s_wifi_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_password_screen, LV_OBJ_FLAG_HIDDEN);
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

    // Settings list rows' description subtitles - live, like the mockup's
    // "value" line, not decorative filler text.
    if (snapshot.state == WIFI_MANAGER_STATE_CONNECTED)
    {
        lv_label_set_text_fmt(s_settings_wifi_row_desc, "Connected to %s", snapshot.active_ssid);
    }
    else
    {
        lv_label_set_text(s_settings_wifi_row_desc, "Not connected");
    }
    lv_label_set_text(s_settings_locale_row_desc, s_locale.time_24h ? "24-hour clock" : "12-hour clock");
}

static void update_wifi_screen(void); // defined further down, alongside the rest of the Wi-Fi screen
static void wifi_password_poll(void); // defined further down, alongside the rest of the password screen

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

    // Only worth refreshing (scan results, connection state) while the
    // user is actually looking at it.
    if (s_active_screen == DISPLAY_UI_SCREEN_SETTINGS && s_settings_view == SETTINGS_VIEW_WIFI)
    {
        update_wifi_screen();
    }
    else if (s_active_screen == DISPLAY_UI_SCREEN_SETTINGS && s_settings_view == SETTINGS_VIEW_WIFI_PASSWORD)
    {
        wifi_password_poll();
    }
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
    // Fixed (not content-fitting) width: "Settings"/"Exit" must not resize
    // the button (and shift its position) when the label text changes.
    lv_obj_set_width(nav_btn, NAV_BUTTON_WIDTH_PX);
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
    // Muted, not accent - this is secondary nav text (matches the mockup's
    // own subheader back-arrow color), not an interactive accent highlight.
    lv_obj_set_style_text_color(s_nav_label, COLOR_MUTED, 0);
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

// lv_switch/lv_textarea/lv_keyboard all render with LVGL's built-in default
// theme unless restyled - light/blue chrome that clashes badly with this
// screen's near-black background. These three helpers are this file's only
// native (non-fully-custom) widgets, so their dark-theme styling lives here
// rather than repeated per call site.
static void style_dark_switch(lv_obj_t *sw)
{
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2A2F38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, COLOR_UP, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COLOR_INK, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
}

static void style_dark_textarea(lv_obj_t *ta)
{
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x171B21), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x2C3440), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 10, 0);
    lv_obj_set_style_text_color(ta, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ta, COLOR_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
    // A thin 2px I-beam, not a filled block - and gated to LV_STATE_FOCUSED
    // specifically, otherwise it was rendering as a static, non-blinking
    // artifact on whichever field *didn't* have focus too (the un-gated
    // LV_PART_CURSOR selector applies regardless of focus state).
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, COLOR_MUTED, LV_PART_CURSOR | LV_STATE_FOCUSED); // matches the eye icon's color
}

static void style_dark_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, COLOR_STATUSBAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1B1F26), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, COLOR_HAIRLINE, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(0x04141C), LV_PART_ITEMS | LV_STATE_CHECKED);
}

// A tappable row matching the reviewed design: a 40x40 icon chip, a title +
// description column, and a chevron. Returns the description label so the
// caller can keep it updated with live state (Wi-Fi status, clock format).
static lv_obj_t *build_settings_row(lv_obj_t *parent, const char *icon_symbol, const char *title, const char *desc,
                                     lv_event_cb_t click_cb)
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
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *chip = lv_obj_create(row);
    lv_obj_remove_style_all(chip);
    make_plain_container(chip);
    lv_obj_set_size(chip, SETTINGS_ICON_CHIP_PX, SETTINGS_ICON_CHIP_PX);
    lv_obj_set_style_radius(chip, 10, 0);
    lv_obj_set_style_bg_color(chip, COLOR_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(chip);
    lv_obj_set_style_text_color(icon, COLOR_ACCENT, 0); // the one place accent-blue is correct: a chip, not nav text
    lv_label_set_text(icon, icon_symbol);

    lv_obj_t *body = lv_obj_create(row);
    lv_obj_remove_style_all(body);
    make_plain_container(body);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 5, 0);

    lv_obj_t *title_label = lv_label_create(body);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(title_label, title);

    lv_obj_t *desc_label = lv_label_create(body);
    lv_obj_set_style_text_color(desc_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(desc_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(desc_label, desc);

    lv_obj_t *chevron = lv_label_create(row);
    lv_obj_set_style_text_color(chevron, COLOR_MUTED, 0);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);

    return desc_label;
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
    lv_obj_set_style_text_color(back_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_16, 0);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);

    lv_obj_t *title_label = lv_label_create(header);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(title_label, title);
}

// Shared by both the SSID field (only when editable - the "Add Network"
// flow) and the password field: links whichever textarea was tapped to the
// one shared keyboard instance.
static void wifi_field_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_wifi_password_keyboard, ta);
    lv_obj_remove_flag(s_wifi_password_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// Toggles whether the SSID/password fields and keyboard accept input -
// used to lock everything down while a connect attempt is in flight (see
// wifi_password_connect_cb()/wifi_password_poll()) so the user can't edit
// out from under an in-progress attempt.
static void wifi_password_set_inputs_enabled(bool enabled)
{
    if (enabled)
    {
        lv_obj_clear_state(s_wifi_password_input, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(s_wifi_password_input, LV_STATE_DISABLED);
    }

    if (!s_wifi_ssid_known)
    {
        if (enabled)
        {
            lv_obj_clear_state(s_wifi_ssid_input, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(s_wifi_ssid_input, LV_STATE_DISABLED);
        }
    }

    // Stays visible while connecting (per feedback: don't hide it) - just
    // disabled, and wifi_keyboard_event_cb() ignores presses while
    // s_wifi_connecting is set, as a second guard since LV_STATE_DISABLED
    // alone doesn't stop lv_keyboard's direct textarea manipulation.
    if (enabled)
    {
        lv_obj_clear_state(s_wifi_password_keyboard, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(s_wifi_password_keyboard, LV_STATE_DISABLED);
    }
}

static void wifi_password_connect_failed(const char *msg)
{
    s_wifi_connecting = false;
    wifi_password_set_inputs_enabled(true);
    lv_label_set_text(s_wifi_password_status, msg);
}

// Called every ~1s from update_timer_cb() while this screen is showing and
// a connect attempt is in flight. wifi_manager can't be polled via its
// event queue here - that queue has exactly one real consumer
// (app_state_sync_task, see its comment) - so this instead watches
// wifi_manager_snapshot_t's last_event_seq, which mirrors the same events
// for read-only observers like this one.
static void wifi_password_poll(void)
{
    if (!s_wifi_connecting)
    {
        return;
    }

    wifi_manager_snapshot_t snap;
    if (wifi_manager_get_snapshot(&snap) != ESP_OK)
    {
        return;
    }

    s_wifi_connect_ticks++;

    if (snap.last_event_seq != s_wifi_connect_baseline_seq)
    {
        switch (snap.last_event)
        {
        case WIFI_MANAGER_EVENT_CONNECTED:
            s_wifi_connecting = false;
            wifi_password_set_inputs_enabled(true);
            show_settings_view(SETTINGS_VIEW_WIFI); // status line there reflects "Connected"
            return;
        case WIFI_MANAGER_EVENT_AUTH_FAILED:
            wifi_password_connect_failed("Incorrect password. Try again.");
            return;
        case WIFI_MANAGER_EVENT_CONNECT_FAILED:
            wifi_password_connect_failed("Could not connect to this network. Try again.");
            return;
        case WIFI_MANAGER_EVENT_DISCONNECTED:
        case WIFI_MANAGER_EVENT_ALL_PROFILES_BLOCKED:
            wifi_password_connect_failed("Connection failed. Try again.");
            return;
        default:
            break; // unrelated event (e.g. a background scan) - keep waiting
        }
    }

    if (s_wifi_connect_ticks > WIFI_CONNECT_TIMEOUT_TICKS)
    {
        wifi_password_connect_failed("Connection timed out. Try again.");
    }
}

// Fires from the keyboard's own accent "Connect" key (see
// s_wifi_kb_map_lc/uc below and wifi_keyboard_event_cb()) - the mockup has
// no separate submit button on this screen, just that one key. Locks the
// fields/keyboard and waits for wifi_password_poll() to see the attempt
// resolve rather than navigating away immediately, so a wrong password
// shows an error right here instead of silently failing on the Wi-Fi list.
static void wifi_password_connect_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_connecting)
    {
        return;
    }

    wifi_manager_snapshot_t snap;
    s_wifi_connect_baseline_seq = (wifi_manager_get_snapshot(&snap) == ESP_OK) ? snap.last_event_seq : 0;
    s_wifi_connect_ticks = 0;
    s_wifi_connecting = true;
    wifi_password_set_inputs_enabled(false);
    lv_label_set_text(s_wifi_password_status, "Connecting...");

    const char *ssid = lv_textarea_get_text(s_wifi_ssid_input);
    strncpy(s_wifi_pending_ssid, ssid, WIFI_MANAGER_SSID_MAX);
    s_wifi_pending_ssid[WIFI_MANAGER_SSID_MAX] = '\0';
    wifi_manager_connect_new(s_wifi_pending_ssid, lv_textarea_get_text(s_wifi_password_input));
}

// Sets up the SSID field for either flow: a known network tapped from the
// scan list (read-only, pre-filled) or the "Add Network" entry point
// (empty, editable via the shared keyboard).
static void wifi_password_screen_set_ssid(const char *ssid, bool known)
{
    s_wifi_ssid_known = known;
    lv_textarea_set_text(s_wifi_ssid_input, ssid);
    lv_label_set_text(s_wifi_password_status, "");

    if (known)
    {
        lv_obj_remove_flag(s_wifi_ssid_input, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_text_color(s_wifi_ssid_input, COLOR_MUTED, 0);
    }
    else
    {
        lv_obj_add_flag(s_wifi_ssid_input, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_text_color(s_wifi_ssid_input, COLOR_TEXT, 0);
    }
}

static void wifi_password_eye_toggle_cb(lv_event_t *e)
{
    lv_obj_t *icon = lv_obj_get_child(lv_event_get_target(e), 0);
    bool now_masked = !lv_textarea_get_password_mode(s_wifi_password_input);
    lv_textarea_set_password_mode(s_wifi_password_input, now_masked);
    lv_label_set_text(icon, now_masked ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

// Custom keyboard maps for Wi-Fi password entry: letters and two symbol modes.
// No cursor keys (touchscreen), single "Connect" key for submission. Mode
// buttons ("ABC"/"abc"/"123"/"#+=") are fully owned by wifi_keyboard_event_cb()
// below rather than LVGL's built-in string-matched dispatch, so the same
// labels can drive full-mode switches (letters <-> symbols) as well as the
// plain shift toggle.

// Custom mode IDs beyond LVGL's built-in TEXT_LOWER/TEXT_UPPER, used with
// lv_keyboard_set_mode() below.
#define WIFI_KB_MODE_SYM_1 2
#define WIFI_KB_MODE_SYM_2 3

// Text mode - lowercase letters
static const char *const s_wifi_kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "123", " ", "Connect", "",
};

// Text mode - uppercase letters
static const char *const s_wifi_kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "123", " ", "Connect", "",
};

// Symbol mode 1: digits + common password/SSID symbols. Row 2/3 symbol
// counts (9/5) are chosen so each row's button-width unit total matches
// (row 2: 9x1=9 units; row 3: shift(2)+5x1+backspace(2)=9 units), keeping
// row 2 and row 3 keys visually similar width.
static const char *const s_wifi_kb_map_sym_1[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "_", ".", ",", ":", ";", "@", "(", ")", "\n",
    "#+=", "'", "\"", "!", "?", "*", LV_SYMBOL_BACKSPACE, "\n",
    "abc", " ", "Connect", "",
};

// Symbol mode 2: remaining symbols, same row shape as mode 1.
static const char *const s_wifi_kb_map_sym_2[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "/", "\\", "+", "=", "<", ">", "#", "[", "]", "\n",
    "!?*", "{", "}", "%", "&", "$", LV_SYMBOL_BACKSPACE, "\n",
    "abc", " ", "Connect", "",
};

// Button widths: 1 = normal key, 2 = shift/mode-switch/backspace, 5 = space,
// 3 = Connect. All buttons share one uniform color (no CUSTOM_1/2 flags).
static const lv_buttonmatrix_ctrl_t s_wifi_kb_ctrl_map[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // row 1: 10 keys
    1, 1, 1, 1, 1, 1, 1, 1, 1,    // row 2: 9 keys
    2, 1, 1, 1, 1, 1, 1, 1, 2,    // row 3: shift (2x), 7 keys (1x), backspace (2x)
    2, 5, 3, 1,                  // row 4: mode-switch (2x), space (5x), Connect (3x), padding
};

// Symbol keyboards' row 2 is 9 keys (letters' is 9 too, but row 3 only has
// 5 symbols instead of letters' 7), so they need their own ctrl map.
static const lv_buttonmatrix_ctrl_t s_wifi_kb_ctrl_map_sym[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // row 1: 10 keys
    1, 1, 1, 1, 1, 1, 1, 1, 1,    // row 2: 9 keys
    2, 1, 1, 1, 1, 1, 2,          // row 3: mode-switch (2x), 5 keys (1x), backspace (2x)
    2, 5, 3, 1,                  // row 4: "abc" (2x), space (5x), Connect (3x), padding
};

// Replaces lv_keyboard_def_event_cb() as the keyboard's LV_EVENT_VALUE_CHANGED
// handler. Owns every mode-switch button ("ABC"/"abc"/"123"/"#+=") and
// "Connect" directly instead of relying on LVGL's built-in string-matched
// shift dispatch, since some of these labels now trigger a full mode change
// (e.g. "abc" from a symbols page back to letters) rather than just a
// lowercase/uppercase toggle. Only plain letters/space/backspace fall
// through to the default handler.
static void wifi_keyboard_event_cb(lv_event_t *e)
{
    if (s_wifi_connecting)
    {
        return; // kept visible per feedback, but inert while an attempt is in flight
    }

    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t btn_id = lv_buttonmatrix_get_selected_button(kb);
    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE)
    {
        return;
    }

    const char *txt = lv_buttonmatrix_get_button_text(kb, btn_id);
    if (txt == NULL)
    {
        return;
    }

    if (strcmp(txt, "Connect") == 0)
    {
        wifi_password_connect_cb(e);
        return;
    }
    if (strcmp(txt, "ABC") == 0)
    {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
        return;
    }
    if (strcmp(txt, "abc") == 0)
    {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        return;
    }
    if (strcmp(txt, "123") == 0 || strcmp(txt, "!?*") == 0)
    {
        lv_keyboard_set_mode(kb, WIFI_KB_MODE_SYM_1);
        return;
    }
    if (strcmp(txt, "#+=") == 0)
    {
        lv_keyboard_set_mode(kb, WIFI_KB_MODE_SYM_2);
        return;
    }

    // Delegate everything else: letters, space, backspace
    lv_keyboard_def_event_cb(e);
}

// All keyboard buttons use uniform styling (no per-button coloring).
// style_dark_keyboard() provides the single look for the entire keyboard.

// ap points into the static s_wifi_snapshot (see its declaration) - stable
// as long as this row exists, which is only ever within the same tick that
// filled it (update_wifi_screen() rebuilds every row on every refresh).
static void wifi_ap_click_cb(lv_event_t *e)
{
    const wifi_manager_ap_t *ap = (const wifi_manager_ap_t *)lv_event_get_user_data(e);
    if (ap->connected)
    {
        return; // already active
    }
    if (ap->saved)
    {
        wifi_manager_connect_saved(ap->ssid);
        return;
    }

    strncpy(s_wifi_pending_ssid, ap->ssid, WIFI_MANAGER_SSID_MAX);
    s_wifi_pending_ssid[WIFI_MANAGER_SSID_MAX] = '\0';
    lv_textarea_set_text(s_wifi_password_input, "");
    wifi_password_screen_set_ssid(s_wifi_pending_ssid, true);
    show_settings_view(SETTINGS_VIEW_WIFI_PASSWORD);
}

// Entry point for the "Add network" row (build_wifi_add_network_row()) -
// unlike wifi_ap_click_cb(), the SSID isn't known yet, so the field starts
// empty and editable.
static void wifi_add_network_click_cb(lv_event_t *e)
{
    (void)e;
    s_wifi_pending_ssid[0] = '\0';
    lv_textarea_set_text(s_wifi_password_input, "");
    wifi_password_screen_set_ssid("", false);
    show_settings_view(SETTINGS_VIEW_WIFI_PASSWORD);
}

// RSSI (dBm, typically -30..-90; higher/less negative is stronger) mapped to
// a 4-bar signal icon using common OS-convention thresholds.
static uint8_t wifi_rssi_to_bars(int8_t rssi)
{
    if (rssi >= -55)
    {
        return 4;
    }
    if (rssi >= -67)
    {
        return 3;
    }
    if (rssi >= -78)
    {
        return 2;
    }
    return 1;
}

// Four bars of increasing height, bottom-aligned. Lit bars are COLOR_ACCENT
// when this is the active connection (mockup shows an accent signal icon on
// the connected row) or COLOR_TEXT otherwise; unlit bars are COLOR_HAIRLINE.
static lv_obj_t *build_signal_icon(lv_obj_t *parent, uint8_t bars, bool active)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    make_plain_container(box);
    lv_obj_set_size(box, 18, 14);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    static const uint8_t heights[4] = {4, 7, 10, 13};
    for (uint8_t i = 0; i < 4; i++)
    {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        make_plain_container(bar);
        lv_obj_set_size(bar, 3, heights[i]);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, (i < bars) ? (active ? COLOR_ACCENT : COLOR_TEXT) : COLOR_HAIRLINE, 0);
    }
    return box;
}

// Custom-drawn: no lock glyph exists in the vendored symbol font (checked -
// LV_SYMBOL_* has none). An lv_arc top half-circle as the shackle plus a
// small rounded rect as the body, both COLOR_MUTED to match the mockup's
// muted lock icon. Only ever created for secured networks.
static lv_obj_t *build_lock_icon(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    make_plain_container(box);
    lv_obj_set_size(box, 14, 14);

    lv_obj_t *shackle = lv_arc_create(box);
    lv_obj_remove_flag(shackle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(shackle, 8, 8);
    lv_obj_align(shackle, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_bg_angles(shackle, 180, 360); // top half-circle
    lv_arc_set_value(shackle, 100);          // indicator covers the same range as bg, so it reads as one stroke
    lv_obj_set_style_arc_width(shackle, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(shackle, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(shackle, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_arc_color(shackle, COLOR_MUTED, LV_PART_INDICATOR);
    // The arc's knob is drawn as an always-on lv_draw_rect regardless of
    // width/height styles (its area comes from LV_PART_KNOB padding, not
    // size) - transparent bg + no border is what actually hides it.
    lv_obj_set_style_bg_opa(shackle, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(shackle, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(shackle, 0, LV_PART_KNOB);

    lv_obj_t *body = lv_obj_create(box);
    lv_obj_remove_style_all(body);
    make_plain_container(body);
    lv_obj_set_size(body, 12, 8);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_set_style_bg_color(body, COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    return box;
}

// Entry point into the "Add Network" flow (empty, editable SSID field) -
// the "Add hidden network" affordance from the original mockup, previously
// a known gap (see the dashboard-design skill's "Known, deliberate gaps").
static void build_wifi_add_network_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), 60);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);
    lv_obj_add_event_cb(row, wifi_add_network_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_style_text_color(icon, COLOR_MUTED, 0); // matches the back-arrow color
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
    lv_label_set_text(icon, LV_SYMBOL_PLUS);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, COLOR_MUTED, 0); // matches the back-arrow color
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_label_set_text(label, "Add network");
}

static void build_wifi_ap_row(const wifi_manager_ap_t *ap)
{
    lv_obj_t *row = lv_obj_create(s_wifi_list);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), 60);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);
    lv_obj_add_event_cb(row, wifi_ap_click_cb, LV_EVENT_CLICKED, (void *)ap);

    lv_obj_t *ssid_label = lv_label_create(row);
    lv_obj_set_style_text_color(ssid_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(ssid_label, ap->ssid);

    // Groups the signal/lock icons with the status text so the outer row's
    // two-slot space-between layout (ssid on the left, this on the right)
    // doesn't need to change.
    lv_obj_t *right_group = lv_obj_create(row);
    lv_obj_remove_style_all(right_group);
    make_plain_container(right_group);
    lv_obj_set_size(right_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_group, 10, 0);

    build_signal_icon(right_group, wifi_rssi_to_bars(ap->rssi), ap->connected);

    lv_obj_t *status_label = lv_label_create(right_group);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    if (ap->connected)
    {
        lv_obj_set_style_text_color(status_label, COLOR_UP, 0);
        lv_label_set_text(status_label, "Connected");
    }
    else if (ap->saved)
    {
        lv_obj_set_style_text_color(status_label, COLOR_MUTED, 0);
        lv_label_set_text(status_label, "Saved");
    }
    else
    {
        lv_obj_set_style_text_color(status_label, COLOR_MUTED, 0);
        lv_label_set_text(status_label, "Tap to connect");
    }

    if (ap->secured)
    {
        build_lock_icon(right_group);
    }
}

// Rebuilds the whole AP list every call rather than diffing - simplest
// correct option, and only runs while the Wi-Fi screen is actually visible
// (see update_timer_cb()), so the cost is bounded to whenever the user is
// looking at this screen. Connection status itself lives only in the
// per-row "Connected"/"Saved"/"Tap to connect" text and the bottom status
// bar - no separate status banner, matching the reviewed design.
#define WIFI_AUTO_RESCAN_TICKS 7 // ~7s between automatic background rescans

static void update_wifi_screen(void)
{
    static uint8_t s_rescan_tick;
    if (++s_rescan_tick >= WIFI_AUTO_RESCAN_TICKS)
    {
        s_rescan_tick = 0;
        wifi_manager_scan_async();
    }

    if (wifi_manager_get_snapshot(&s_wifi_snapshot) != ESP_OK)
    {
        return;
    }

    lv_obj_clean(s_wifi_list);
    build_wifi_add_network_row(s_wifi_list);
    if (s_wifi_snapshot.ap_count == 0)
    {
        lv_obj_t *empty = lv_label_create(s_wifi_list);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_obj_set_style_pad_top(empty, 16, 0);
        lv_label_set_text(empty, "Scanning...");
        return;
    }
    for (uint8_t i = 0; i < s_wifi_snapshot.ap_count; i++)
    {
        build_wifi_ap_row(&s_wifi_snapshot.aps[i]);
    }
}

static void wifi_row_click_cb(lv_event_t *e)
{
    (void)e;
    wifi_manager_scan_async();
    show_settings_view(SETTINGS_VIEW_WIFI);
}

static void build_wifi_screen(lv_obj_t *screen)
{
    s_wifi_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_wifi_screen);
    make_plain_container(s_wifi_screen);
    lv_obj_set_size(s_wifi_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_wifi_screen, LV_FLEX_FLOW_COLUMN);

    build_subscreen_header(s_wifi_screen, "Wi-Fi", settings_back_cb);

    // Matches the mockup's .section-label: a small uppercase muted heading
    // above the network list, not a status banner.
    lv_obj_t *section_label = lv_label_create(s_wifi_screen);
    lv_obj_set_style_text_color(section_label, lv_color_hex(0x565C67), 0);
    lv_obj_set_style_text_font(section_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_left(section_label, 20, 0);
    lv_obj_set_style_pad_top(section_label, 14, 0);
    lv_obj_set_style_pad_bottom(section_label, 6, 0);
    lv_label_set_text(section_label, "NETWORKS");

    s_wifi_list = lv_obj_create(s_wifi_screen);
    lv_obj_remove_style_all(s_wifi_list);
    // Unlike this file's other plain containers, this one genuinely needs to
    // scroll - the AP list can run longer than the screen - so only the
    // flags that don't apply to a scrollable list are stripped.
    lv_obj_remove_flag(s_wifi_list, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_PRESS_LOCK |
                                         LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_width(s_wifi_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_wifi_list, 1);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
}

static void wifi_password_back_cb(lv_event_t *e)
{
    (void)e;
    // Leaving mid-attempt shouldn't leave the fields disabled for next time -
    // the connect call already in flight still runs to completion in
    // wifi_manager, this just stops this screen watching for its result.
    s_wifi_connecting = false;
    wifi_password_set_inputs_enabled(true);
    show_settings_view(SETTINGS_VIEW_WIFI);
}

static void build_wifi_password_screen(lv_obj_t *screen)
{
    s_wifi_password_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_wifi_password_screen);
    make_plain_container(s_wifi_password_screen);
    lv_obj_set_size(s_wifi_password_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_wifi_password_screen, LV_FLEX_FLOW_COLUMN);

    build_subscreen_header(s_wifi_password_screen, "Add Network", wifi_password_back_cb);

    // SSID + password fields sit vertically centered in the space between
    // the header and the keyboard: this wrapper takes all the leftover
    // flex space (flex_grow 1) and centers its children within it.
    lv_obj_t *fields_wrap = lv_obj_create(s_wifi_password_screen);
    lv_obj_remove_style_all(fields_wrap);
    make_plain_container(fields_wrap);
    lv_obj_set_size(fields_wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(fields_wrap, 1);
    lv_obj_set_flex_flow(fields_wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(fields_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Field rows are wider than the original 90%-of-screen sizing (see the
    // password field's own +20px below) so the SSID and password boxes
    // still line up edge-to-edge.
    const int32_t field_row_width_px = (int32_t)(BOARD_JC4880P443C_LCD_H_RES * 9 / 10) + 20;
    // Vertically centers the one-line text within the fixed-height field
    // (WIFI_PASSWORD_FIELD_HEIGHT_PX) instead of a fixed top-only pad.
    const int32_t field_vpad_px = (WIFI_PASSWORD_FIELD_HEIGHT_PX - lv_font_get_line_height(&lv_font_montserrat_16)) / 2;

    lv_obj_t *ssid_label = lv_label_create(fields_wrap);
    lv_obj_set_style_text_color(ssid_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(ssid_label, 1, 0);
    lv_obj_set_style_pad_left(ssid_label, 20, 0);
    lv_label_set_text(ssid_label, "SSID");

    lv_obj_t *ssid_field_row = lv_obj_create(fields_wrap);
    lv_obj_remove_style_all(ssid_field_row);
    make_plain_container(ssid_field_row);
    lv_obj_set_size(ssid_field_row, field_row_width_px, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(ssid_field_row, 20, 0);
    lv_obj_set_style_margin_top(ssid_field_row, 8, 0);

    s_wifi_ssid_input = lv_textarea_create(ssid_field_row);
    style_dark_textarea(s_wifi_ssid_input);
    lv_textarea_set_one_line(s_wifi_ssid_input, true);
    lv_textarea_set_max_length(s_wifi_ssid_input, WIFI_MANAGER_SSID_MAX);
    lv_obj_set_width(s_wifi_ssid_input, LV_PCT(100));
    // Fixed height matching the eye button below, no border.
    lv_obj_set_height(s_wifi_ssid_input, WIFI_PASSWORD_FIELD_HEIGHT_PX);
    lv_obj_set_style_pad_top(s_wifi_ssid_input, field_vpad_px, 0);
    lv_obj_set_style_pad_bottom(s_wifi_ssid_input, field_vpad_px, 0);
    lv_obj_set_style_border_width(s_wifi_ssid_input, 0, 0);
    lv_obj_add_event_cb(s_wifi_ssid_input, wifi_field_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *password_label = lv_label_create(fields_wrap);
    lv_obj_set_style_text_color(password_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(password_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(password_label, 1, 0);
    lv_obj_set_style_pad_top(password_label, 40, 0); // +20px extra gap between the SSID and password groups
    lv_obj_set_style_pad_left(password_label, 20, 0);
    lv_label_set_text(password_label, "PASSWORD");

    // Textarea + eye-toggle button share a row so the icon sits inline at
    // the field's right edge, matching the mockup's .pwd-row.
    lv_obj_t *field_row = lv_obj_create(fields_wrap);
    lv_obj_remove_style_all(field_row);
    make_plain_container(field_row);
    lv_obj_set_size(field_row, field_row_width_px, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(field_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(field_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_left(field_row, 20, 0);
    lv_obj_set_style_margin_top(field_row, 8, 0);
    lv_obj_set_style_pad_column(field_row, 8, 0);

    s_wifi_password_input = lv_textarea_create(field_row);
    style_dark_textarea(s_wifi_password_input);
    lv_textarea_set_one_line(s_wifi_password_input, true);
    lv_textarea_set_password_mode(s_wifi_password_input, true);
    lv_textarea_set_max_length(s_wifi_password_input, WIFI_MANAGER_PASSWORD_MAX);
    lv_obj_set_flex_grow(s_wifi_password_input, 1);
    // Same fixed height as the SSID field above, matching the eye button.
    lv_obj_set_height(s_wifi_password_input, WIFI_PASSWORD_FIELD_HEIGHT_PX);
    lv_obj_set_style_pad_top(s_wifi_password_input, field_vpad_px, 0);
    lv_obj_set_style_pad_bottom(s_wifi_password_input, field_vpad_px, 0);
    lv_obj_set_style_border_width(s_wifi_password_input, 0, 0);
    lv_obj_add_event_cb(s_wifi_password_input, wifi_field_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_password_input, wifi_password_connect_cb, LV_EVENT_READY, NULL);

    lv_obj_t *eye_btn = lv_button_create(field_row);
    lv_obj_remove_style_all(eye_btn);
    make_plain_container(eye_btn);
    lv_obj_add_flag(eye_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(eye_btn, WIFI_PASSWORD_FIELD_HEIGHT_PX, WIFI_PASSWORD_FIELD_HEIGHT_PX);
    lv_obj_set_ext_click_area(eye_btn, 6);
    lv_obj_set_flex_flow(eye_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eye_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Styled as its own chip matching the textarea's background, rather
    // than floating bare over the screen background.
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(0x171B21), 0);
    lv_obj_set_style_bg_opa(eye_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye_btn, 10, 0);
    lv_obj_add_event_cb(eye_btn, wifi_password_eye_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *eye_icon = lv_label_create(eye_btn);
    lv_obj_set_style_text_color(eye_icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(eye_icon, &lv_font_montserrat_18, 0);
    lv_label_set_text(eye_icon, LV_SYMBOL_EYE_CLOSE); // starts masked, matches lv_textarea_set_password_mode(true) above

    // Connecting/error status - replaces a static hint, only shown once a
    // connect attempt is under way (see wifi_password_connect_cb()/
    // wifi_password_poll()).
    s_wifi_password_status = lv_label_create(fields_wrap);
    lv_obj_set_style_text_color(s_wifi_password_status, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_wifi_password_status, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_wifi_password_status, field_row_width_px);
    lv_label_set_long_mode(s_wifi_password_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_margin_left(s_wifi_password_status, 20, 0);
    lv_obj_set_style_margin_top(s_wifi_password_status, 10, 0);
    lv_label_set_text(s_wifi_password_status, "");

    s_wifi_password_keyboard = lv_keyboard_create(s_wifi_password_screen);
    style_dark_keyboard(s_wifi_password_keyboard);
    // Default height is 50% of parent (LV_PCT(50), see lv_keyboard_class in
    // LVGL); shrink 50px shorter than that to leave more room above it.
    lv_obj_set_height(s_wifi_password_keyboard,
                       (BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX) / 2 - 50);

    // Set up all four keyboard modes: text lower/upper, symbols mode 1, symbols mode 2
    lv_keyboard_set_map(s_wifi_password_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, s_wifi_kb_map_lc, s_wifi_kb_ctrl_map);
    lv_keyboard_set_map(s_wifi_password_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, s_wifi_kb_map_uc, s_wifi_kb_ctrl_map);
    lv_keyboard_set_map(s_wifi_password_keyboard, WIFI_KB_MODE_SYM_1, s_wifi_kb_map_sym_1, s_wifi_kb_ctrl_map_sym);
    lv_keyboard_set_map(s_wifi_password_keyboard, WIFI_KB_MODE_SYM_2, s_wifi_kb_map_sym_2, s_wifi_kb_ctrl_map_sym);

    lv_keyboard_set_mode(s_wifi_password_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_event_cb(s_wifi_password_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_wifi_password_keyboard, wifi_keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_wifi_password_keyboard, LV_OBJ_FLAG_HIDDEN);
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

    // Top-level header: title only, no back arrow (unlike sub-screens - this
    // *is* the top level within Settings).
    lv_obj_t *header = lv_obj_create(s_settings_list);
    lv_obj_remove_style_all(header);
    make_plain_container(header);
    lv_obj_set_size(header, LV_PCT(100), SETTINGS_LIST_HEADER_HEIGHT_PX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(header, 20, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, COLOR_HAIRLINE, 0);

    lv_obj_t *header_title = lv_label_create(header);
    lv_obj_set_style_text_color(header_title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(header_title, &lv_font_montserrat_18, 0);
    lv_label_set_text(header_title, "Settings");

    s_settings_wifi_row_desc =
        build_settings_row(s_settings_list, LV_SYMBOL_WIFI, "Wi-Fi", "--", wifi_row_click_cb);
    // LVGL's built-in symbol set has no clock/calendar glyph - the gear is
    // the closest available placeholder, not a dedicated "date & time" icon.
    s_settings_locale_row_desc =
        build_settings_row(s_settings_list, LV_SYMBOL_SETTINGS, "Date & time", "--", locale_row_click_cb);
}

static void locale_24h_toggle_cb(lv_event_t *e)
{
    lv_obj_t *toggle = lv_event_get_target(e);
    s_locale.time_24h = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    settings_store_save_locale(&s_locale);
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
    style_dark_switch(toggle);
    if (s_locale.time_24h)
    {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(toggle, locale_24h_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
    // Time zone editing (raw POSIX TZ string) removed for now - revisiting
    // the approach later; the switch above is all this screen does today.
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
    build_wifi_screen(screen);
    build_wifi_password_screen(screen);
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

#if CONFIG_DEV_SCREENSHOT_CONSOLE

// Dev-only: jumps straight to a named screen so tools/dev_screenshot.py
// --nav can drive screenshot-based UI verification without a physical tap.
// Reuses set_active_screen()/show_settings_view() - the same functions the
// real touch handlers call - so navigation here can't drift from what a tap
// actually does. Runs on the console REPL task, not the LVGL task, so every
// lv_obj_* call below must happen under the same display lock cmd_screenshot
// already uses for the same reason.
static int cmd_nav(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("NAV_ERROR reason=missing_target\n");
        return 1;
    }
    const char *target = argv[1];

    if (!board_jc4880p443c_display_lock(0))
    {
        printf("NAV_ERROR reason=lock_failed\n");
        return 1;
    }

    if (strcmp(target, "watchlist") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_WATCHLIST);
    }
    else if (strcmp(target, "settings") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
    }
    else if (strcmp(target, "wifi") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        wifi_manager_scan_async(); // matches wifi_row_click_cb(), populates the list instead of "Scanning..."
        show_settings_view(SETTINGS_VIEW_WIFI);
    }
    else if (strcmp(target, "wifi_password") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);

        // Purely for visual capture, not a real connect attempt - always
        // takes this branch regardless of whether ssid matches a nearby AP
        // (unlike wifi_ap_click_cb(), which only reaches this screen for
        // networks that are neither already-connected nor already-saved).
        const char *ssid = (argc >= 3) ? argv[2] : "Test Network";
        strncpy(s_wifi_pending_ssid, ssid, WIFI_MANAGER_SSID_MAX);
        s_wifi_pending_ssid[WIFI_MANAGER_SSID_MAX] = '\0';
        lv_textarea_set_text(s_wifi_password_input, "");
        wifi_password_screen_set_ssid(s_wifi_pending_ssid, true);
        show_settings_view(SETTINGS_VIEW_WIFI_PASSWORD);

        // Mirrors wifi_field_focus_cb() - a real tap on the textarea would
        // show the keyboard too, and this screen reads as empty without it.
        lv_keyboard_set_textarea(s_wifi_password_keyboard, s_wifi_password_input);
        lv_obj_remove_flag(s_wifi_password_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        board_jc4880p443c_display_unlock();
        printf("NAV_ERROR reason=unknown_target\n");
        return 1;
    }

    board_jc4880p443c_display_unlock();
    printf("NAV_OK target=%s\n", target);
    return 0;
}

esp_err_t display_ui_register_dev_nav_console(void)
{
    const esp_console_cmd_t nav_cmd = {
        .command = "nav",
        .help = "Jump directly to a screen: watchlist | settings | wifi | wifi_password [ssid] (dev builds only)",
        .hint = NULL,
        .func = &cmd_nav,
    };
    return esp_console_cmd_register(&nav_cmd);
}

#else // !CONFIG_DEV_SCREENSHOT_CONSOLE

esp_err_t display_ui_register_dev_nav_console(void)
{
    return ESP_OK;
}

#endif
