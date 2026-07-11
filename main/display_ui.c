#include "display_ui.h"

#include "app_state.h"
#include "app_state_sync_task.h"
#include "board_jc4880p443c.h"
#include "display_format.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ota_client.h"
#include "sdkconfig.h"
#include "settings_api_region_map.h"
#include "settings_store.h"
#include "time_sync.h"
#include "wifi_manager.h"

#if CONFIG_DEV_SCREENSHOT_CONSOLE
#include "esp_console.h"
#endif

#include <ctype.h>
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
#define SETTINGS_ROW_HEIGHT_PX 108
#define WATCHLIST_ROW_HEIGHT_PX 58
#define SETTINGS_LIST_HEADER_HEIGHT_PX 64
#define SUBHEADER_HEIGHT_PX 64
#define SETTINGS_ICON_CHIP_PX 40
#define TIME_LIST_ROW_HEIGHT_PX 60 // Time format / Date format / Time zones list rows
#define WIFI_PASSWORD_FIELD_HEIGHT_PX 54 // matches the eye-toggle button below it
#define UPDATE_PERIOD_MS 1000
// Fixed integer range fed to lv_chart_set_axis_range()/set_series_ext_y_array().
// Sparkline points are normalized into this range via
// display_format_normalize_value() rather than a fixed "* 100 cents" scale,
// so low-priced symbols (sub-cent movement) don't collapse into a flat or
// 0/1-jittering line - see docs/testing.md's display_format entry.
#define CHART_SCALE_MAX 10000
// (field_row_width_px = LCD_H_RES*9/10 + 20 = 452) minus the match card's
// 14px side padding, split across the 3 stat columns in build_watchlist_stat().
#define WATCHLIST_MATCH_STAT_VALUE_WIDTH_PX 140

#define COLOR_INK lv_color_hex(0x0A0C10)
#define COLOR_STATUSBAR_BG lv_color_hex(0x0F1216)
#define COLOR_TEXT lv_color_hex(0xECEEF2)
#define COLOR_MUTED lv_color_hex(0x6E7686)
#define COLOR_MUTED_DIM lv_color_hex(0x454C59) // darker than COLOR_MUTED - low-emphasis chrome like the drag-handle dots
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

// Which Settings sub-screen is showing.
typedef enum
{
    SETTINGS_VIEW_LIST = 0,
    SETTINGS_VIEW_LOCALE,
    SETTINGS_VIEW_TIME_FORMAT,
    SETTINGS_VIEW_DATE_FORMAT,
    SETTINGS_VIEW_TIME_ZONES,
    SETTINGS_VIEW_TIME_ZONE_CITIES,
    SETTINGS_VIEW_REGION,
    SETTINGS_VIEW_WIFI,
    SETTINGS_VIEW_WIFI_PASSWORD,
    SETTINGS_VIEW_WATCHLIST_MANAGE,
    SETTINGS_VIEW_WATCHLIST_ADD,
    SETTINGS_VIEW_UPDATES,
    SETTINGS_VIEW_ABOUT,
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
static lv_obj_t *s_settings_updates_row_desc;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_statusbar_signal_bars[4]; // status bar's wifi signal icon, bars[0..3] short-to-tall
static lv_obj_t *s_nav_label;
static display_ui_screen_t s_active_screen;
static settings_view_t s_settings_view;

static lv_obj_t *s_locale_screen;

// Settings > Time > Time format / Date format / Time zones. Time format and
// Date format are small fixed lists built once; each row's checkmark is a
// child created up front and just hidden/shown on selection. Time zones is
// two screens (region list, then that region's cities) - the city list's
// content depends on which region was tapped, so it's cleared and rebuilt
// each time (see show_time_zone_cities()).
static lv_obj_t *s_time_format_screen;
static lv_obj_t *s_time_format_check_12h;
static lv_obj_t *s_time_format_check_24h;
static lv_obj_t *s_date_format_screen;
static lv_obj_t *s_time_zone_screen;
static lv_obj_t *s_time_zone_city_screen;
static lv_obj_t *s_time_zone_city_list;
static lv_obj_t *s_time_zone_city_subtitle;
static lv_obj_t *s_region_screen;
static lv_obj_t *s_region_subtitle;
static lv_obj_t *s_region_check_auto;
static lv_obj_t *s_region_check_intl;
static lv_obj_t *s_region_check_us;

// Loaded once at startup; kept up to date by the Time format/Date
// format/Time zones screens as they save changes.
static locale_settings_t s_locale;

// Composed/sorted view of one network for display: the connected network
// first, then every saved profile (in scan range or not), then whatever's
// left from the scan - see build_wifi_display_rows(). Kept separate from
// wifi_manager_ap_t since a saved-but-out-of-range profile has no scan
// data (rssi/secured) to point to.
typedef struct
{
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    int8_t rssi;
    bool has_rssi; // false for a saved network currently out of scan range
    bool saved;
    bool connected;
    bool secured;
} wifi_display_row_t;

// Per-row click context - just the fields wifi_ap_click_cb() needs, so a
// row's event callback stays valid independent of the display-row array's
// own lifetime/comparison use.
typedef struct
{
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    bool saved;
    bool connected;
} wifi_row_click_ctx_t;

static lv_obj_t *s_wifi_screen;
static lv_obj_t *s_wifi_list;
static wifi_manager_snapshot_t s_wifi_snapshot;

// Last set of rows actually rendered into s_wifi_list, so update_wifi_screen()
// can skip the teardown/rebuild when nothing changed - see its definition for
// why that matters (rebuilding on every tick could destroy a row mid-tap).
#define WIFI_DISPLAY_ROWS_MAX (WIFI_MANAGER_MAX_SCAN_APS + WIFI_MANAGER_MAX_PROFILES)
static wifi_display_row_t s_wifi_rendered_rows[WIFI_DISPLAY_ROWS_MAX];
static uint8_t s_wifi_rendered_count;

// Per-row click context, indexed the same as the row it was created for.
// Static (not stack-local) so each row's click handler can safely reference
// "its" ssid/saved/connected by pointer after the function that built the
// row returns - see build_wifi_ap_row()/wifi_ap_click_cb().
static wifi_row_click_ctx_t s_wifi_click_ctx[WIFI_DISPLAY_ROWS_MAX];

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

// --- Watchlist symbols (Settings > Watchlist symbols) ---

static lv_obj_t *s_settings_watchlist_row_desc;

static lv_obj_t *s_watchlist_manage_screen;
static lv_obj_t *s_watchlist_manage_subtitle;
static lv_obj_t *s_watchlist_list;

// Per-row click context, indexed the same as the row it was created for -
// same rationale as s_wifi_click_ctx: stays valid for the row's remove
// button and drag handle after build_watchlist_symbol_row() returns. `row`
// is needed by the drag handlers below to reposition/query this specific
// row's lv_obj_t during a drag.
typedef struct
{
    uint8_t index;
    lv_obj_t *row;
} watchlist_row_click_ctx_t;
static watchlist_row_click_ctx_t s_watchlist_click_ctx[SETTINGS_MAX_WATCHLIST];

// Drag-and-drop reorder state for the watchlist Manage screen. Singleton,
// not per-row: only one drag can be in flight at a time (single-touch GT911
// panel). Auto-scroll-while-dragging near the list's viewport edges is a
// deliberate omission - at the SETTINGS_MAX_WATCHLIST cap, all rows plus the
// "Add symbol" pill still fit within the screen without scrolling, so it
// isn't needed in practice.
typedef struct
{
    bool active;
    lv_obj_t *row;
    uint8_t start_index;
    int32_t start_point_y;
    int32_t baseline_y; // advances by consumed row-height steps - see watchlist_drag_pressing_cb()
} watchlist_drag_state_t;
static watchlist_drag_state_t s_watchlist_drag;

static lv_obj_t *s_watchlist_add_screen;
static lv_obj_t *s_watchlist_add_subtitle;
static lv_obj_t *s_watchlist_symbol_input;
static lv_obj_t *s_watchlist_add_keyboard;
static lv_obj_t *s_watchlist_status_label; // "Searching..." shown only while a check is in flight
static lv_obj_t *s_watchlist_match_card;
static lv_obj_t *s_watchlist_match_pair_label;
static lv_obj_t *s_watchlist_match_last_price_label;
static lv_obj_t *s_watchlist_match_change_label;
static lv_obj_t *s_watchlist_match_range_label;
static lv_obj_t *s_watchlist_error_note;
static lv_obj_t *s_watchlist_error_label;
static lv_obj_t *s_watchlist_add_button;

// Set by watchlist_add_check_cb() on a successful "Search" lookup;
// consumed by watchlist_add_to_watchlist_cb() so the symbol actually added
// is always the last one that round-tripped Binance, not just whatever text
// currently sits in the field (which could have been edited since).
static char s_watchlist_pending_symbol[SETTINGS_SYMBOL_MAX_LEN + 1];
static bool s_watchlist_match_valid;

// --- Updates (Settings > Updates) ---

static lv_obj_t *s_updates_screen;
static lv_obj_t *s_updates_state_card;    // tinted card - color/icon/text set per-state in updates_screen_reset()
static lv_obj_t *s_updates_state_icon;
static lv_obj_t *s_updates_state_text;
static lv_obj_t *s_updates_current_version_label;
static lv_obj_t *s_updates_latest_version_label;
static lv_obj_t *s_updates_last_checked_label;
static lv_obj_t *s_updates_status_label; // "Installing...", or an error - hidden otherwise
static lv_obj_t *s_updates_progress_row;   // bar + percent label, shown only while installing
static lv_obj_t *s_updates_progress_bar;
static lv_obj_t *s_updates_progress_label;
// One button, context-dependent - "Check for updates" normally, "Install
// update" once app_state_get_ota_info() reports update_available, same
// single-button-changes-label idiom as the bottom nav button
// (set_active_screen()'s s_nav_label). Its own label text also becomes
// "Checking..." while a check is in flight - see updates_action_cb().
static lv_obj_t *s_updates_action_button;
static lv_obj_t *s_updates_action_label;

// --- About (Settings > About) ---

static lv_obj_t *s_about_screen;

// Reused scratch buffers: avoids per-tick dynamic allocation (AGENTS.md: no
// dynamic allocation in the hot path) and avoids putting
// sizeof(market_data_kline_t) * APP_STATE_KLINE_CAPACITY (~25KB) on the LVGL
// port task's stack. Safe because update_row() runs rows strictly one at a
// time within a single timer callback - no concurrent use.
static market_data_kline_t s_klines_scratch[APP_STATE_KLINE_CAPACITY];
static int32_t s_chart_scratch[APP_STATE_MAX_SYMBOLS][APP_STATE_KLINE_CAPACITY];

// Buckets the time since event_ms (an app_state_ota_info_t.last_check_ms
// value) into a coarse "X ago" label - event_ms == 0 means never. No
// existing relative-time formatter in this file to reuse (update_statusbar()
// only ever formats wall-clock time via strftime).
static void format_relative_time(int64_t event_ms, char *out, size_t out_len)
{
    if (event_ms == 0)
    {
        snprintf(out, out_len, "Never");
        return;
    }

    int64_t delta_s = (esp_timer_get_time() / 1000 - event_ms) / 1000;
    if (delta_s < 60)
    {
        snprintf(out, out_len, "Just now");
    }
    else if (delta_s < 3600)
    {
        snprintf(out, out_len, "%d min ago", (int)(delta_s / 60));
    }
    else if (delta_s < 86400)
    {
        snprintf(out, out_len, "%d h ago", (int)(delta_s / 3600));
    }
    else
    {
        snprintf(out, out_len, "%d d ago", (int)(delta_s / 86400));
    }
}

static const char *ota_err_message(ota_client_err_t err)
{
    switch (err)
    {
    case OTA_CLIENT_ERR_NOT_SYNCED:
        return "Clock not synced yet - try again shortly.";
    case OTA_CLIENT_ERR_NETWORK:
    case OTA_CLIENT_ERR_TIMEOUT:
        return "Couldn't reach GitHub. Try again.";
    case OTA_CLIENT_ERR_HTTP_STATUS:
    case OTA_CLIENT_ERR_PARSE:
        return "Unexpected response from GitHub. Try again.";
    case OTA_CLIENT_ERR_NO_MEM:
        return "Not enough memory right now. Try again.";
    case OTA_CLIENT_ERR_OTA_FAILED:
        return "Update failed - firmware not changed.";
    default:
        return "Something went wrong. Try again.";
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
    // Fixed width so the label never bleeds into the chart column. The text
    // itself is always composed as an explicit two-line "L <lo>\nH <hi>"
    // (see update_row()), so every row's range label is the same height
    // regardless of how many decimals a symbol needs - LONG_DOT is just a
    // defensive fallback in case a single line is ever wider than this column.
    lv_obj_set_width(row->range_label, ROW_SIDE_COL_WIDTH_PX);
    lv_label_set_long_mode(row->range_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(row->range_label, "L --\nH --");

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
    // Fixed once at setup - point values are pre-normalized into this same
    // range in update_row() via display_format_normalize_value(), so the
    // axis itself never needs to change per tick.
    lv_chart_set_axis_range(row->chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_SCALE_MAX);
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
    // Same overflow safety net as range_label above, kept right-aligned to
    // match the pre-existing flush-right look for short prices.
    lv_obj_set_width(row->price_label, ROW_SIDE_COL_WIDTH_PX);
    lv_obj_set_style_text_align(row->price_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(row->price_label, LV_LABEL_LONG_DOT);

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
        // Invalid-symbol responses get their own muted "Unsupported" label,
        // distinct from the transient-looking orange "Unavailable" used for
        // every other unrecoverable error - this one won't clear itself
        // until the region changes (see app_state_reset_symbols_for_region_change).
        bool unsupported = (meta.last_error == MARKET_DATA_ERR_SYMBOL_NOT_FOUND);
        lv_label_set_text(row->price_label, "");
        lv_obj_set_style_text_color(row->change_label, unsupported ? COLOR_MUTED : COLOR_WARN, 0);
        lv_label_set_text(row->change_label, unsupported ? "Unsupported" : "Unavailable");
        return; // keep the last-known range/chart, if any, for context
    }

    if (meta.kline_count == 0)
    {
        // Never synced yet (INIT), or DEGRADED before a first sync ever
        // succeeded - nothing to compute a price/range/chart from.
        lv_label_set_text(row->range_label, "L --\nH --");
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
    // Explicit "L <lo>\nH <hi>" - always two lines, low-to-high order with
    // letter labels so the reading direction never needs to be memorized.
    // An embedded newline (rather than a single "hi / lo" line left to
    // LVGL's auto-wrap) guarantees the break always falls between the two
    // values instead of mid-digit, and keeps every row the same height.
    char range_buf[2 * sizeof(hi_buf) + 8];
    char change_buf[16];
    display_format_price(last_close, price_buf, sizeof(price_buf));
    display_format_price(hi, hi_buf, sizeof(hi_buf));
    display_format_price(lo, lo_buf, sizeof(lo_buf));
    snprintf(range_buf, sizeof(range_buf), "L %s\nH %s", lo_buf, hi_buf);
    snprintf(change_buf, sizeof(change_buf), "%+.2f%%", change_pct);

    lv_label_set_text(row->range_label, range_buf);
    lv_chart_set_series_color(row->chart, row->series, up ? COLOR_UP : COLOR_DOWN);

    int32_t *chart_data = s_chart_scratch[index];
    for (uint16_t i = 0; i < count; i++)
    {
        chart_data[i] = display_format_normalize_value(s_klines_scratch[i].close, lo, hi, CHART_SCALE_MAX);
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
    lv_obj_add_flag(s_time_format_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_date_format_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_time_zone_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_time_zone_city_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_region_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_password_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_watchlist_manage_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_watchlist_add_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_updates_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_about_screen, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *target = s_settings_list;
    switch (view)
    {
    case SETTINGS_VIEW_LOCALE:
        target = s_locale_screen;
        break;
    case SETTINGS_VIEW_TIME_FORMAT:
        target = s_time_format_screen;
        break;
    case SETTINGS_VIEW_DATE_FORMAT:
        target = s_date_format_screen;
        break;
    case SETTINGS_VIEW_TIME_ZONES:
        target = s_time_zone_screen;
        break;
    case SETTINGS_VIEW_TIME_ZONE_CITIES:
        target = s_time_zone_city_screen;
        break;
    case SETTINGS_VIEW_REGION:
        target = s_region_screen;
        break;
    case SETTINGS_VIEW_WIFI:
        target = s_wifi_screen;
        break;
    case SETTINGS_VIEW_WIFI_PASSWORD:
        target = s_wifi_password_screen;
        break;
    case SETTINGS_VIEW_WATCHLIST_MANAGE:
        target = s_watchlist_manage_screen;
        break;
    case SETTINGS_VIEW_WATCHLIST_ADD:
        target = s_watchlist_add_screen;
        break;
    case SETTINGS_VIEW_UPDATES:
        target = s_updates_screen;
        break;
    case SETTINGS_VIEW_ABOUT:
        target = s_about_screen;
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
        lv_obj_add_flag(s_time_format_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_date_format_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_zone_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_zone_city_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_region_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_password_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_watchlist_manage_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_watchlist_add_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_updates_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_about_screen, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_nav_label, "SETTINGS");
    }
    else
    {
        lv_obj_add_flag(s_rows_container, LV_OBJ_FLAG_HIDDEN);
        show_settings_view(SETTINGS_VIEW_LIST); // always re-enter Settings at its top level
        lv_label_set_text(s_nav_label, "EXIT");
    }
}

// One button, context-dependent: "Settings" while on the watchlist, "Exit"
// (back to the watchlist, from anywhere in Settings) while on Settings -
// not two persistent tabs. Each Settings sub-screen has its own back
// control for stepping back one level instead.
static void nav_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_active_screen == DISPLAY_UI_SCREEN_SETTINGS && s_settings_view == SETTINGS_VIEW_WIFI)
    {
        wifi_manager_resume_autoconnect();
    }
    set_active_screen(s_active_screen == DISPLAY_UI_SCREEN_WATCHLIST ? DISPLAY_UI_SCREEN_SETTINGS
                                                                      : DISPLAY_UI_SCREEN_WATCHLIST);
}

static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_settings_view == SETTINGS_VIEW_WIFI)
    {
        wifi_manager_resume_autoconnect();
    }
    show_settings_view(SETTINGS_VIEW_LIST);
}

static void tz_describe(const char *tz_label, const char *posix, char *out, size_t out_len); // with the Time screens, below
static bool wifi_ap_is_live_connected(const wifi_manager_snapshot_t *snap, const char *ssid); // with the Wi-Fi screens, below
static uint8_t wifi_rssi_to_bars(int8_t rssi);                                                // with the Wi-Fi screens, below

// Looks up the RSSI of the currently-connected network from the latest scan
// pass, for the status bar's signal icon. Same staleness caveat as
// wifi_ap_is_live_connected() - see its comment - but for a single icon that
// only distinguishes "no signal"/"full bars" this is imperceptible in
// practice.
static bool statusbar_lookup_connected_rssi(const wifi_manager_snapshot_t *snap, int8_t *out_rssi)
{
    for (uint8_t i = 0; i < snap->ap_count; i++)
    {
        if (wifi_ap_is_live_connected(snap, snap->aps[i].ssid))
        {
            *out_rssi = snap->aps[i].rssi;
            return true;
        }
    }
    return false;
}

// Sets the status bar's 4-bar signal icon: `bars` (0-4) lit in `color`,
// the rest dimmed to COLOR_HAIRLINE - mirrors build_signal_icon()'s look
// (see below) without needing to rebuild the icon on every update.
static void statusbar_set_signal_bars(uint8_t bars, lv_color_t color)
{
    for (uint8_t i = 0; i < 4; i++)
    {
        lv_obj_set_style_bg_color(s_statusbar_signal_bars[i], (i < bars) ? color : COLOR_HAIRLINE, 0);
    }
}

static void update_statusbar(void)
{
    if (time_sync_is_synced())
    {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        char clock_fmt[32];
        const char *date_fmt = s_locale.date_format[0] != '\0' ? s_locale.date_format : "%d %b %Y";
        snprintf(clock_fmt, sizeof(clock_fmt), "%s  %s", date_fmt, s_locale.time_24h ? "%H:%M" : "%I:%M %p");

        char clock_buf[40];
        strftime(clock_buf, sizeof(clock_buf), clock_fmt, &tm_now);
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
    {
        int8_t rssi;
        // Edge case at 1b) in build_wifi_display_rows() below: the manager
        // considers a profile connected but the last scan pass hasn't caught
        // up yet - no rssi to size the icon by, so show full bars rather
        // than none (an empty icon reads as "no signal", not "unknown").
        uint8_t bars = statusbar_lookup_connected_rssi(&snapshot, &rssi) ? wifi_rssi_to_bars(rssi) : 4;
        // COLOR_MUTED, not COLOR_UP - matches the SETTINGS/EXIT label next to
        // it rather than reading as its own accent color.
        statusbar_set_signal_bars(bars, COLOR_MUTED);
        break;
    }
    case WIFI_MANAGER_STATE_CONNECTING:
        statusbar_set_signal_bars(2, COLOR_WARN);
        break;
    case WIFI_MANAGER_STATE_ERROR:
        statusbar_set_signal_bars(1, COLOR_WARN);
        break;
    default:
        statusbar_set_signal_bars(0, COLOR_HAIRLINE);
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
    char tz_desc[40];
    tz_describe(s_locale.tz_label, s_locale.posix_tz, tz_desc, sizeof(tz_desc));
    // Plain ASCII separator, not a middle-dot - see build_wifi_ap_row()'s
    // "Saved - Not in range" for why (vendored Montserrat has no such glyph).
    lv_label_set_text_fmt(s_settings_locale_row_desc, "%s - %s", s_locale.time_24h ? "24-hour" : "12-hour", tz_desc);

    uint8_t watchlist_count = app_state_symbol_count();
    if (watchlist_count > SETTINGS_MAX_WATCHLIST)
    {
        watchlist_count = SETTINGS_MAX_WATCHLIST; // defensive; settings_store already bounds this
    }
    lv_label_set_text_fmt(s_settings_watchlist_row_desc, "%u of %u Binance pairs", (unsigned)watchlist_count,
                           (unsigned)SETTINGS_MAX_WATCHLIST);

    app_state_ota_info_t ota_info;
    app_state_get_ota_info(&ota_info);
    if (ota_info.latest_version[0] == '\0')
    {
        // app_state_ota_info_t's documented all-zero "never checked" state -
        // avoid the false "up to date" claim the raw update_available=false
        // flag would otherwise imply.
        lv_label_set_text(s_settings_updates_row_desc, "Not checked yet");
    }
    else if (ota_info.update_available)
    {
        lv_label_set_text_fmt(s_settings_updates_row_desc, "Update available: %s", ota_info.latest_version);
    }
    else
    {
        lv_label_set_text_fmt(s_settings_updates_row_desc, "Up to date (%s)", ota_info.latest_version);
    }
}

static void update_wifi_screen(void); // defined further down, alongside the rest of the Wi-Fi screen
static void wifi_password_poll(void); // defined further down, alongside the rest of the password screen

// Mutually referenced across the Watchlist manage/add screens (the add
// screen's "Add to watchlist" rebuilds the manage list; the manage screen's
// "Add symbol" row resets the add screen) - forward-declared here rather
// than reordering the two screens' otherwise-independent code.
static void watchlist_manage_rebuild(void);
static void watchlist_add_screen_reset(void);

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

// Same 4-bar signal glyph as build_signal_icon() (see the Wi-Fi list rows,
// below) but keeps a pointer to each bar in s_statusbar_signal_bars so
// update_statusbar() can recolor them in place instead of rebuilding the
// icon on every tick.
static void build_statusbar_signal_icon(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE |
                                 LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                 LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_WITH_ARROW |
                                 LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(box, 24, 18);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    static const uint8_t heights[4] = {5, 9, 13, 17};
    for (uint8_t i = 0; i < 4; i++)
    {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE |
                                     LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                     LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_WITH_ARROW |
                                     LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_size(bar, 4, heights[i]);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, COLOR_HAIRLINE, 0);
        s_statusbar_signal_bars[i] = bar;
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
    // left unset, "Exit" (wider than "Settings") would overflow past that
    // fixed box and clip on the left since this container is packed flush
    // to the bar's right edge.
    lv_obj_set_width(right, LV_SIZE_CONTENT);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 16, 0);

    build_statusbar_signal_icon(right);

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
    // the bar's full height instead; remove_style_all also dropped the
    // button's own default centering layout, so that's reapplied explicitly
    // too.
    lv_obj_set_height(nav_btn, STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(nav_btn, LV_FLEX_FLOW_ROW);
    // Right-aligned (not centered): with a fixed width below, this pins the
    // label to the button's right edge so SETTINGS/EXIT stay flush with the
    // bar's pad_right - same margin the clock label has from pad_left -
    // regardless of which (differently-sized) text is showing.
    lv_obj_set_flex_align(nav_btn, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // A label-sized hit target is as hard to tap as a hyperlink - pad the
    // button past its text, then extend the touch-sensitive area further
    // still, without changing how big it looks.
    lv_obj_set_style_pad_top(nav_btn, 4, 0);
    lv_obj_set_style_pad_bottom(nav_btn, 4, 0);
    lv_obj_set_style_pad_left(nav_btn, 10, 0);
    lv_obj_set_ext_click_area(nav_btn, 16);
    // Fixed width sized to the wider of "SETTINGS"/"EXIT" (plus the tap-area
    // padding above) - not content-fitting. Swapping between the two texts
    // must not resize the button, since that would also shift the signal
    // icon next to it (this container's other child, packed against the
    // same right-aligned edge).
    lv_point_t settings_sz;
    lv_point_t exit_sz;
    lv_text_get_size(&settings_sz, "SETTINGS", &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&exit_sz, "EXIT", &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t nav_label_w = settings_sz.x > exit_sz.x ? settings_sz.x : exit_sz.x;
    lv_obj_set_width(nav_btn, nav_label_w + 10 /* pad_left above */);
    lv_obj_add_event_cb(nav_btn, nav_click_cb, LV_EVENT_CLICKED, NULL);
    s_nav_label = lv_label_create(nav_btn);
    // Muted, not accent - this is secondary nav text (matches the mockup's
    // own subheader back-arrow color), not an interactive accent highlight.
    lv_obj_set_style_text_color(s_nav_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_nav_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_nav_label, "SETTINGS");
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

// lv_textarea/lv_keyboard both render with LVGL's built-in default theme
// unless restyled - light/blue chrome that clashes badly with this screen's
// near-black background. These two helpers are this file's only native
// (non-fully-custom) widgets, so their dark-theme styling lives here rather
// than repeated per call site.
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

    // Without an explicit LV_STATE_DISABLED style, LVGL's base theme washes
    // the field out to a light gray while a connect attempt is in flight
    // (see wifi_password_set_inputs_enabled()) - keep the same bg/border and
    // only mute the text color instead, matching how a read-only known-SSID
    // field already looks (see wifi_password_screen_set_ssid()).
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x171B21), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x2C3440), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(ta, COLOR_MUTED, LV_STATE_DISABLED);
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
// reviewed design. Reused by every Settings sub-screen. subtitle is
// optional (pass NULL for a title-only header, as every screen before
// Watchlist symbols does); when given, it's rendered under the title and
// the created label is returned so the caller can keep it live-updated
// (e.g. "N of 10") - NULL is returned when subtitle is NULL.
static lv_obj_t *build_subscreen_header(lv_obj_t *parent, const char *title, const char *subtitle,
                                         lv_event_cb_t back_cb)
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

    if (subtitle == NULL)
    {
        lv_obj_t *title_label = lv_label_create(header);
        lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
        lv_label_set_text(title_label, title);
        return NULL;
    }

    lv_obj_t *titles = lv_obj_create(header);
    lv_obj_remove_style_all(titles);
    make_plain_container(titles);
    lv_obj_set_size(titles, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(titles, 2, 0);

    lv_obj_t *title_label = lv_label_create(titles);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(title_label, title);

    lv_obj_t *subtitle_label = lv_label_create(titles);
    lv_obj_set_style_text_color(subtitle_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(subtitle_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(subtitle_label, subtitle);
    return subtitle_label;
}

// A plain tappable row: title on the left, chevron on the right - no icon
// chip or description. Used by Settings > Time's root screen and the Time
// zones region list, where build_settings_row()'s icon chip would be
// redundant repeated three/eight times on one screen.
static lv_obj_t *build_nav_row(lv_obj_t *parent, const char *title, lv_event_cb_t click_cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    // CLICKABLE without SCROLLABLE, so a drag starting on this row must
    // chain up to the scrollable body list instead - but make_plain_container()
    // also strips SCROLL_CHAIN, and LVGL's scroll-object search
    // (lv_indev_find_scroll_obj()) stops at the very first non-scrollable
    // ancestor that lacks it, never reaching that scrollable parent. Add it
    // back or every row-filled list here is unscrollable past its own
    // height - see build_time_subscreen()'s body.
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(row, LV_PCT(100), TIME_LIST_ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *title_label = lv_label_create(row);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(title_label, title);

    lv_obj_t *chevron = lv_label_create(row);
    lv_obj_set_style_text_color(chevron, COLOR_MUTED, 0);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);

    return row;
}

// A tappable row for a single choice in a list of mutually-exclusive
// options: title on the left, a checkmark on the right when is_current is
// true. Used by the Time zone city list, which is rebuilt from scratch per
// region (see show_time_zone_cities()) so it can't pre-create hidden
// checkmarks the way the fixed-length Time format/Date format lists do.
static lv_obj_t *build_selectable_row(lv_obj_t *parent, const char *title, bool is_current, lv_event_cb_t click_cb,
                                       void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    // See build_nav_row()'s comment on LV_OBJ_FLAG_SCROLL_CHAIN - without it
    // a drag starting on this row can't reach the scrollable parent list.
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(row, LV_PCT(100), TIME_LIST_ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *title_label = lv_label_create(row);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(title_label, title);

    if (is_current)
    {
        lv_obj_t *check = lv_label_create(row);
        lv_obj_set_style_text_color(check, COLOR_ACCENT, 0);
        lv_label_set_text(check, LV_SYMBOL_OK);
    }

    return row;
}

// A Time sub-screen's shared skeleton: a plain fixed-height column holding
// build_subscreen_header() plus a scrollable body flex column that the
// caller fills with rows (build_nav_row()/build_selectable_row()). Mirrors
// the outer-plain/inner-scrollable split already used by the Wi-Fi and
// Watchlist manage screens (see s_wifi_list/s_watchlist_list).
static lv_obj_t *build_time_subscreen(lv_obj_t *screen, lv_obj_t **out_screen, const char *title,
                                       const char *subtitle, lv_obj_t **out_subtitle_label, lv_event_cb_t back_cb)
{
    lv_obj_t *sub = lv_obj_create(screen);
    lv_obj_remove_style_all(sub);
    make_plain_container(sub);
    lv_obj_set_size(sub, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_COLUMN);
    if (out_screen)
    {
        *out_screen = sub;
    }

    lv_obj_t *subtitle_label = build_subscreen_header(sub, title, subtitle, back_cb);
    if (out_subtitle_label)
    {
        *out_subtitle_label = subtitle_label;
    }

    lv_obj_t *body = lv_obj_create(sub);
    lv_obj_remove_style_all(body);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_PRESS_LOCK |
                                  LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_width(body, LV_PCT(100));
    // An explicit height, not flex_grow: with flex_grow, LVGL sized this to
    // its *content* (every row added) rather than clipping to the space
    // left under the header, so a full list rendered past the status bar
    // with nothing left to actually scroll - no overflow, no scrollbar.
    lv_obj_set_height(body, BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX - SUBHEADER_HEIGHT_PX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    return body;
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
// lv_keyboard_set_mode() below. lv_keyboard_set_map() writes into LVGL's
// shared *global* mode->map table (kb_map[]/kb_ctrl[] in lv_keyboard.c;
// there is no per-object storage), so every custom mode ID used anywhere in
// the app must be unique across *all* keyboards, not just within this one -
// reusing an ID (e.g. matching WATCHLIST_KB_MODE_SYM_1/2 below) lets
// whichever screen is built last silently overwrite the other's symbol-page
// map. USER_1/2 vs the watchlist keyboard's USER_3/4 keep the two apart.
#define WIFI_KB_MODE_SYM_1 LV_KEYBOARD_MODE_USER_1
#define WIFI_KB_MODE_SYM_2 LV_KEYBOARD_MODE_USER_2

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
    // Every branch below re-asserts this keyboard's own map/ctrl-map right
    // after lv_keyboard_set_mode() rather than trusting its result: LVGL
    // stores per-mode maps in one *global* table shared by every lv_keyboard
    // in the app (kb_map[]/kb_ctrl[] in lv_keyboard.c - there is no
    // per-object storage, even for the built-in TEXT_LOWER/TEXT_UPPER
    // modes), so whichever keyboard last called lv_keyboard_set_map() for a
    // given mode silently wins for every other keyboard using that same
    // mode too. The watchlist "Add symbol" keyboard shares TEXT_LOWER/UPPER
    // with this one and is built after it, so without this override,
    // switching case here would render *its* "Search" action key instead of
    // "Connect".
    if (strcmp(txt, "ABC") == 0)
    {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
        lv_buttonmatrix_set_map(kb, s_wifi_kb_map_uc);
        lv_buttonmatrix_set_ctrl_map(kb, s_wifi_kb_ctrl_map);
        return;
    }
    if (strcmp(txt, "abc") == 0)
    {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_buttonmatrix_set_map(kb, s_wifi_kb_map_lc);
        lv_buttonmatrix_set_ctrl_map(kb, s_wifi_kb_ctrl_map);
        return;
    }
    if (strcmp(txt, "123") == 0 || strcmp(txt, "!?*") == 0)
    {
        lv_keyboard_set_mode(kb, WIFI_KB_MODE_SYM_1);
        lv_buttonmatrix_set_map(kb, s_wifi_kb_map_sym_1);
        lv_buttonmatrix_set_ctrl_map(kb, s_wifi_kb_ctrl_map_sym);
        return;
    }
    if (strcmp(txt, "#+=") == 0)
    {
        lv_keyboard_set_mode(kb, WIFI_KB_MODE_SYM_2);
        lv_buttonmatrix_set_map(kb, s_wifi_kb_map_sym_2);
        lv_buttonmatrix_set_ctrl_map(kb, s_wifi_kb_ctrl_map_sym);
        return;
    }

    // Delegate everything else: letters, space, backspace
    lv_keyboard_def_event_cb(e);
}

// All keyboard buttons use uniform styling (no per-button coloring).
// style_dark_keyboard() provides the single look for the entire keyboard.

// Wires the keyboard to `field` and focuses it immediately, instead of
// waiting for the user to tap the field first (wifi_field_focus_cb() does
// the same two calls on LV_EVENT_FOCUSED - this is that same behavior
// applied up front, on screen entry, so the keyboard is never the first
// thing missing from an otherwise-empty-looking screen).
static void wifi_password_screen_focus_default_field(lv_obj_t *field)
{
    lv_obj_add_state(field, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(s_wifi_password_keyboard, field);
    lv_obj_remove_flag(s_wifi_password_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// ctx points into the static s_wifi_click_ctx array (see its declaration),
// not into wifi_manager_ap_t/s_wifi_snapshot - it stays valid across ticks
// where update_wifi_screen() decides nothing changed and skips rebuilding,
// which is what makes a tap on a saved network register reliably instead of
// needing several attempts (see update_wifi_screen()'s comment).
static void wifi_ap_click_cb(lv_event_t *e)
{
    const wifi_row_click_ctx_t *ctx = (const wifi_row_click_ctx_t *)lv_event_get_user_data(e);
    if (ctx->connected)
    {
        return; // already active
    }
    if (ctx->saved)
    {
        wifi_manager_connect_saved(ctx->ssid);
        return;
    }

    strncpy(s_wifi_pending_ssid, ctx->ssid, WIFI_MANAGER_SSID_MAX);
    s_wifi_pending_ssid[WIFI_MANAGER_SSID_MAX] = '\0';
    lv_textarea_set_text(s_wifi_password_input, "");
    wifi_password_screen_set_ssid(s_wifi_pending_ssid, true);
    show_settings_view(SETTINGS_VIEW_WIFI_PASSWORD);
    wifi_password_screen_focus_default_field(s_wifi_password_input); // SSID is fixed/known - password is the only thing left to type
}

// --- Wi-Fi row context menu (kebab button -> "Forget network") ---
//
// One shared popup, built once in build_wifi_screen() as a sibling of
// s_wifi_list rather than something created per-row - so a list rebuild
// (auto-rescan, or the forget action itself completing) never tears it
// down while it's open. Retargeted on each open via s_wifi_menu_ssid,
// copied out of the row's click ctx rather than keeping a pointer to it,
// since s_wifi_click_ctx slots get overwritten on the next rebuild.
static lv_obj_t *s_wifi_menu_backdrop;
static lv_obj_t *s_wifi_menu_card;
static lv_obj_t *s_wifi_menu_ssid_label;
static char s_wifi_menu_ssid[WIFI_MANAGER_SSID_MAX + 1];

static void wifi_menu_hide(void)
{
    lv_obj_add_flag(s_wifi_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_menu_card, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_menu_backdrop_click_cb(lv_event_t *e)
{
    (void)e;
    wifi_menu_hide();
}

static void wifi_menu_open_cb(lv_event_t *e)
{
    const wifi_row_click_ctx_t *ctx = (const wifi_row_click_ctx_t *)lv_event_get_user_data(e);
    strncpy(s_wifi_menu_ssid, ctx->ssid, WIFI_MANAGER_SSID_MAX);
    s_wifi_menu_ssid[WIFI_MANAGER_SSID_MAX] = '\0';
    lv_label_set_text(s_wifi_menu_ssid_label, s_wifi_menu_ssid);

    lv_obj_remove_flag(s_wifi_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_wifi_menu_card, LV_OBJ_FLAG_HIDDEN);
}

// No confirmation prompt - matches the watchlist remove button's
// convention. update_wifi_screen() polls the snapshot every tick and diffs
// it against what's rendered, so it picks up the profile removal and
// rebuilds the row on its own; no explicit rebuild call needed here.
static void wifi_menu_forget_click_cb(lv_event_t *e)
{
    (void)e;
    wifi_manager_forget(s_wifi_menu_ssid);
    wifi_menu_hide();
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
    wifi_password_screen_focus_default_field(s_wifi_ssid_input); // SSID is empty here - fill it in first
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
    lv_obj_set_size(box, 24, 18);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    static const uint8_t heights[4] = {5, 9, 13, 17};
    for (uint8_t i = 0; i < 4; i++)
    {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        make_plain_container(bar);
        lv_obj_set_size(bar, 4, heights[i]);
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

// Custom-drawn, same reasoning as build_lock_icon(): no vertical "..."
// (kebab/overflow-menu) glyph exists in the vendored symbol font either.
// Three small circles stacked in a column, COLOR_MUTED. Only ever created
// for saved networks - see build_wifi_ap_row()'s menu button.
static lv_obj_t *build_kebab_icon(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    make_plain_container(box);
    lv_obj_set_size(box, 6, 18);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (uint8_t i = 0; i < 3; i++)
    {
        lv_obj_t *dot = lv_obj_create(box);
        lv_obj_remove_style_all(dot);
        make_plain_container(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, COLOR_MUTED, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
    return box;
}

// Entry point into the "Add Network" flow (empty, editable SSID field) -
// the "Add hidden network" affordance from the original mockup. Styled as a
// standalone bordered pill (mockup's .addbtn) below the scanned networks
// rather than a plain list row, so it reads as a distinct action, not just
// another AP entry. The mockup's border is dashed, but the vendored LVGL 9.5
// (see lv_obj_style_gen.h) only exposes border color/opa/width/side/post -
// no border-style/dashed property (dropped from LVGL 9) - so this uses a
// solid border as the closest available approximation.
static void build_wifi_add_network_row(lv_obj_t *parent)
{
    // Explicit width instead of LV_PCT(100), since the row also carries its
    // own left/right margin (mockup: margin: 12px 18px) - LV_PCT is measured
    // against the parent's content box, so combining it with margin would
    // overflow the list by 2x the margin.
    const int32_t addbtn_width_px = BOARD_JC4880P443C_LCD_H_RES - 2 * 18;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, addbtn_width_px, 46);
    lv_obj_set_style_margin_left(row, 18, 0);
    lv_obj_set_style_margin_top(row, 12, 0);
    lv_obj_set_style_margin_bottom(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_radius(row, 10, 0);
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

// Row order matches the mockup's .net-row: signal icon on the left, an
// SSID/status column (SSID on top, status text below it) taking the
// remaining width, and the lock icon (secured networks only) and kebab
// menu button (saved networks only) pinned to the right edge - not
// [ssid] ... [signal, status, lock, kebab] all clustered together.
// `index` selects this row's slot in s_wifi_click_ctx - see that array's
// declaration and wifi_ap_click_cb().
static void build_wifi_ap_row(const wifi_display_row_t *disp_row, uint8_t index)
{
    lv_obj_t *row = lv_obj_create(s_wifi_list);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), 66);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 20, 0);
    lv_obj_set_style_pad_right(row, 20, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);

    wifi_row_click_ctx_t *ctx = &s_wifi_click_ctx[index];
    strncpy(ctx->ssid, disp_row->ssid, WIFI_MANAGER_SSID_MAX);
    ctx->ssid[WIFI_MANAGER_SSID_MAX] = '\0';
    ctx->saved = disp_row->saved;
    ctx->connected = disp_row->connected;
    lv_obj_add_event_cb(row, wifi_ap_click_cb, LV_EVENT_CLICKED, ctx);

    build_signal_icon(row, disp_row->has_rssi ? wifi_rssi_to_bars(disp_row->rssi) : 0, disp_row->connected);

    lv_obj_t *info = lv_obj_create(row);
    lv_obj_remove_style_all(info);
    make_plain_container(info);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *ssid_label = lv_label_create(info);
    lv_obj_set_style_text_color(ssid_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(ssid_label, disp_row->ssid);

    lv_obj_t *status_label = lv_label_create(info);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_top(status_label, 2, 0);
    if (disp_row->connected)
    {
        lv_obj_set_style_text_color(status_label, COLOR_UP, 0);
        lv_label_set_text(status_label, "Connected");
    }
    else if (disp_row->saved)
    {
        lv_obj_set_style_text_color(status_label, COLOR_MUTED, 0);
        // Plain ASCII separator - the vendored Montserrat fonts here only
        // cover CONFIG_LV_FONT_MONTSERRAT's default (Latin/ASCII) range, so a
        // middle-dot ("\xC2\xB7", used elsewhere in the reviewed mockup)
        // renders as a missing-glyph box.
        lv_label_set_text(status_label, disp_row->has_rssi ? "Saved" : "Saved - Not in range");
    }
    else
    {
        lv_obj_set_style_text_color(status_label, COLOR_MUTED, 0);
        lv_label_set_text(status_label, "Tap to connect");
    }

    if (disp_row->secured)
    {
        build_lock_icon(row);
    }

    // Kebab ("...") button, saved networks only - opens the shared context
    // menu (wifi_menu_open_cb(), see its declaration) with "Forget network".
    // Being a clickable child of the (also clickable) row, LVGL dispatches a
    // tap here to this button, not to wifi_ap_click_cb() on the row.
    if (disp_row->saved)
    {
        lv_obj_t *menu_btn = lv_obj_create(row);
        lv_obj_remove_style_all(menu_btn);
        make_plain_container(menu_btn);
        lv_obj_add_flag(menu_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(menu_btn, 32, 40);
        lv_obj_set_flex_flow(menu_btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(menu_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_ext_click_area(menu_btn, 8);
        lv_obj_add_event_cb(menu_btn, wifi_menu_open_cb, LV_EVENT_CLICKED, ctx);
        build_kebab_icon(menu_btn);
    }
}

// wifi_manager_ap_t.connected is baked in at the *last completed scan* (see
// wifi_policy_sort_scan()/handle_scan_done() in wifi_manager.c) - it isn't
// re-derived on every snapshot read. So if the connection changes between
// scans (e.g. disconnect from a saved network, fail to connect elsewhere,
// end up offline), a stale scan can keep reporting the old network as
// connected for up to WIFI_AUTO_RESCAN_TICKS seconds. snapshot.state and
// snapshot.active_ssid, in contrast, ARE recomputed on every snapshot call
// (update_snapshot_from_policy()) - use those instead of ap->connected so a
// row's "Connected" label/accent, and wifi_ap_click_cb's "already active"
// guard, can't get stuck pointing at a network that's no longer actually
// active (which otherwise left that row silently unresponsive to taps).
static bool wifi_ap_is_live_connected(const wifi_manager_snapshot_t *snap, const char *ssid)
{
    return snap->state == WIFI_MANAGER_STATE_CONNECTED && strcmp(snap->active_ssid, ssid) == 0;
}

// Builds the sorted view described at wifi_display_row_t's declaration:
// connected network first, then every saved profile (in scan range or not),
// then whatever else the scan turned up. Returns the row count written to
// out_rows (capacity WIFI_DISPLAY_ROWS_MAX).
static uint8_t build_wifi_display_rows(const wifi_manager_snapshot_t *snap, wifi_display_row_t *out_rows)
{
    uint8_t count = 0;
    bool ap_consumed[WIFI_MANAGER_MAX_SCAN_APS] = {0};
    bool connected_added = false;

    // 1) The connected network, if the latest scan pass still sees it.
    for (uint8_t i = 0; i < snap->ap_count; i++)
    {
        if (!wifi_ap_is_live_connected(snap, snap->aps[i].ssid))
        {
            continue;
        }
        const wifi_manager_ap_t *ap = &snap->aps[i];
        strncpy(out_rows[count].ssid, ap->ssid, WIFI_MANAGER_SSID_MAX);
        out_rows[count].ssid[WIFI_MANAGER_SSID_MAX] = '\0';
        out_rows[count].rssi = ap->rssi;
        out_rows[count].has_rssi = true;
        out_rows[count].saved = ap->saved;
        out_rows[count].connected = true;
        out_rows[count].secured = ap->secured;
        count++;
        ap_consumed[i] = true;
        connected_added = true;
        break; // at most one active connection
    }

    // 1b) Edge case: wifi_manager considers a profile connected but the most
    // recent scan pass didn't include it (e.g. scan hasn't refreshed since
    // connecting) - still surface it at the top rather than dropping it.
    if (!connected_added)
    {
        for (uint8_t k = 0; k < snap->profile_count; k++)
        {
            if (!snap->known[k].connected)
            {
                continue;
            }
            strncpy(out_rows[count].ssid, snap->known[k].ssid, WIFI_MANAGER_SSID_MAX);
            out_rows[count].ssid[WIFI_MANAGER_SSID_MAX] = '\0';
            out_rows[count].rssi = 0;
            out_rows[count].has_rssi = false;
            out_rows[count].saved = true;
            out_rows[count].connected = true;
            out_rows[count].secured = true; // unknown here - see the (3) comment below
            count++;
            connected_added = true;
            break;
        }
    }

    // 2) Remaining saved networks currently in scan range.
    for (uint8_t i = 0; i < snap->ap_count; i++)
    {
        if (ap_consumed[i] || !snap->aps[i].saved)
        {
            continue;
        }
        const wifi_manager_ap_t *ap = &snap->aps[i];
        strncpy(out_rows[count].ssid, ap->ssid, WIFI_MANAGER_SSID_MAX);
        out_rows[count].ssid[WIFI_MANAGER_SSID_MAX] = '\0';
        out_rows[count].rssi = ap->rssi;
        out_rows[count].has_rssi = true;
        out_rows[count].saved = true;
        out_rows[count].connected = false;
        out_rows[count].secured = ap->secured;
        count++;
        ap_consumed[i] = true;
    }

    // 3) Saved profiles the current scan doesn't see at all - kept listed
    // (per request: known networks stay visible even out of range) with no
    // signal bars lit instead of being dropped. wifi_manager_known_t has no
    // security-mode field, so `secured` defaults to true here - nearly all
    // saved home/office networks are secured, and showing a lock on the rare
    // open one is a smaller error than the reverse.
    for (uint8_t k = 0; k < snap->profile_count && count < WIFI_DISPLAY_ROWS_MAX; k++)
    {
        const wifi_manager_known_t *known = &snap->known[k];
        bool already_listed = false;
        for (uint8_t j = 0; j < count; j++)
        {
            if (strcmp(out_rows[j].ssid, known->ssid) == 0)
            {
                already_listed = true;
                break;
            }
        }
        if (already_listed)
        {
            continue;
        }
        strncpy(out_rows[count].ssid, known->ssid, WIFI_MANAGER_SSID_MAX);
        out_rows[count].ssid[WIFI_MANAGER_SSID_MAX] = '\0';
        out_rows[count].rssi = 0;
        out_rows[count].has_rssi = false;
        out_rows[count].saved = true;
        out_rows[count].connected = false;
        out_rows[count].secured = true;
        count++;
    }

    // 4) Everything else the scan turned up - not saved, not connected.
    for (uint8_t i = 0; i < snap->ap_count && count < WIFI_DISPLAY_ROWS_MAX; i++)
    {
        if (ap_consumed[i])
        {
            continue;
        }
        const wifi_manager_ap_t *ap = &snap->aps[i];
        strncpy(out_rows[count].ssid, ap->ssid, WIFI_MANAGER_SSID_MAX);
        out_rows[count].ssid[WIFI_MANAGER_SSID_MAX] = '\0';
        out_rows[count].rssi = ap->rssi;
        out_rows[count].has_rssi = true;
        out_rows[count].saved = false;
        out_rows[count].connected = false;
        out_rows[count].secured = ap->secured;
        count++;
    }

    return count;
}

// Rebuilds s_wifi_list only when build_wifi_display_rows() produces a
// different result than last time (see s_wifi_rendered_rows/_count) rather
// than unconditionally on every ~1s tick. Tearing the whole list down and
// recreating it regardless of whether anything changed meant a tap landing
// mid-rebuild (press registered on a row that gets lv_obj_clean()'d before
// its release fires) was silently dropped - the user had to keep tapping a
// saved network until one attempt happened to land between rebuilds.
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

    wifi_display_row_t new_rows[WIFI_DISPLAY_ROWS_MAX];
    memset(new_rows, 0, sizeof(new_rows)); // zero padding too, so the memcmp below is reliable
    uint8_t new_count = build_wifi_display_rows(&s_wifi_snapshot, new_rows);

    // s_wifi_list starts out with no children at all (build_wifi_screen()
    // creates it empty), so an all-zero new_count on the very first tick
    // would otherwise compare equal to the equally-empty initial cache and
    // skip building anything - not even the "Scanning..."/add-network
    // placeholder. s_wifi_list_built forces the first pass through.
    static bool s_wifi_list_built;
    if (s_wifi_list_built && new_count == s_wifi_rendered_count &&
        memcmp(new_rows, s_wifi_rendered_rows, (size_t)new_count * sizeof(new_rows[0])) == 0)
    {
        return; // nothing changed - leave the existing rows (and any in-progress tap) alone
    }
    memcpy(s_wifi_rendered_rows, new_rows, (size_t)new_count * sizeof(new_rows[0]));
    s_wifi_rendered_count = new_count;
    s_wifi_list_built = true;

    lv_obj_clean(s_wifi_list);
    if (new_count == 0)
    {
        lv_obj_t *empty = lv_label_create(s_wifi_list);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_obj_set_style_pad_top(empty, 16, 0);
        lv_label_set_text(empty, "Scanning...");
        build_wifi_add_network_row(s_wifi_list);
        return;
    }
    for (uint8_t i = 0; i < new_count; i++)
    {
        build_wifi_ap_row(&new_rows[i], i);
    }
    build_wifi_add_network_row(s_wifi_list);
}

static void wifi_row_click_cb(lv_event_t *e)
{
    (void)e;
    wifi_manager_pause_autoconnect(); // don't let autoconnect retries starve this scan
    wifi_manager_scan_async();
    // The context menu's backdrop covers s_wifi_screen but not the bottom
    // nav bar, which lives outside it - so tapping "Exit" while the menu is
    // open leaves it un-hidden. Reset it here, the one external entry point
    // into this screen, so a stale open menu can never reappear next visit.
    wifi_menu_hide();
    show_settings_view(SETTINGS_VIEW_WIFI);
}

static void build_wifi_screen(lv_obj_t *screen)
{
    s_wifi_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_wifi_screen);
    make_plain_container(s_wifi_screen);
    lv_obj_set_size(s_wifi_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_wifi_screen, LV_FLEX_FLOW_COLUMN);

    build_subscreen_header(s_wifi_screen, "Wi-Fi", NULL, settings_back_cb);

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

    // Context menu (backdrop + card), hidden until a row's kebab button
    // opens it - see wifi_menu_open_cb(). Built once here, as the last two
    // children of s_wifi_screen, so plain z-order (LVGL renders children in
    // creation order) puts them above the list without needing
    // lv_obj_move_foreground() on every open. LV_OBJ_FLAG_IGNORE_LAYOUT is
    // required on both: s_wifi_screen is a flex column, and without that
    // flag the flex layout would reposition/reflow these into the column
    // instead of leaving them at their manually-set overlay position.
    s_wifi_menu_backdrop = lv_obj_create(s_wifi_screen);
    lv_obj_remove_style_all(s_wifi_menu_backdrop);
    make_plain_container(s_wifi_menu_backdrop);
    lv_obj_add_flag(s_wifi_menu_backdrop,
                     LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_wifi_menu_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_wifi_menu_backdrop, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_menu_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wifi_menu_backdrop, LV_OPA_50, 0);
    lv_obj_add_event_cb(s_wifi_menu_backdrop, wifi_menu_backdrop_click_cb, LV_EVENT_CLICKED, NULL);

    // Anchored to s_wifi_screen's own bottom edge (its height already stops
    // short of the status/nav bar, see its lv_obj_set_size() call above) so
    // this floats just above the bottom bar, not centered mid-screen.
    s_wifi_menu_card = lv_obj_create(s_wifi_screen);
    lv_obj_remove_style_all(s_wifi_menu_card);
    make_plain_container(s_wifi_menu_card);
    lv_obj_add_flag(s_wifi_menu_card, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_wifi_menu_card, BOARD_JC4880P443C_LCD_H_RES - 2 * 18, LV_SIZE_CONTENT);
    lv_obj_align(s_wifi_menu_card, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_flex_flow(s_wifi_menu_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_wifi_menu_card, lv_color_hex(0x12151A), 0);
    lv_obj_set_style_bg_opa(s_wifi_menu_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_wifi_menu_card, 14, 0);
    lv_obj_set_style_border_width(s_wifi_menu_card, 1, 0);
    lv_obj_set_style_border_color(s_wifi_menu_card, COLOR_HAIRLINE, 0);
    lv_obj_set_style_pad_bottom(s_wifi_menu_card, 8, 0);

    s_wifi_menu_ssid_label = lv_label_create(s_wifi_menu_card);
    lv_obj_set_style_text_color(s_wifi_menu_ssid_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_wifi_menu_ssid_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_top(s_wifi_menu_ssid_label, 16, 0);
    lv_obj_set_style_pad_left(s_wifi_menu_ssid_label, 18, 0);
    lv_obj_set_style_pad_bottom(s_wifi_menu_ssid_label, 8, 0);
    lv_label_set_text(s_wifi_menu_ssid_label, "");

    lv_obj_t *divider = lv_obj_create(s_wifi_menu_card);
    lv_obj_remove_style_all(divider);
    make_plain_container(divider);
    lv_obj_set_size(divider, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(divider, COLOR_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    lv_obj_t *forget_row = lv_obj_create(s_wifi_menu_card);
    lv_obj_remove_style_all(forget_row);
    make_plain_container(forget_row);
    lv_obj_add_flag(forget_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(forget_row, LV_PCT(100), 52);
    lv_obj_set_flex_flow(forget_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(forget_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(forget_row, 18, 0);
    lv_obj_set_style_pad_column(forget_row, 10, 0);
    lv_obj_add_event_cb(forget_row, wifi_menu_forget_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *forget_icon = lv_label_create(forget_row);
    lv_obj_set_style_text_color(forget_icon, COLOR_DOWN, 0);
    lv_label_set_text(forget_icon, LV_SYMBOL_TRASH);

    lv_obj_t *forget_label = lv_label_create(forget_row);
    lv_obj_set_style_text_color(forget_label, COLOR_DOWN, 0);
    lv_obj_set_style_text_font(forget_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(forget_label, "Forget network");
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

    build_subscreen_header(s_wifi_password_screen, "Add Network", NULL, wifi_password_back_cb);

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

// Rebuilds symbol_settings_t from app_state's current in-memory watchlist
// and persists it - app_state_add_symbol()/app_state_remove_symbol() are
// runtime-only (see their doc comments and docs/decisions/0007), so every
// mutation here must be followed by this to survive a reboot.
static void watchlist_save_current_symbols(void)
{
    symbol_settings_t cfg;
    settings_symbols_init_default(&cfg);

    uint8_t count = app_state_symbol_count();
    if (count > SETTINGS_MAX_WATCHLIST)
    {
        count = SETTINGS_MAX_WATCHLIST;
    }
    cfg.count = count;
    for (uint8_t i = 0; i < count; i++)
    {
        app_state_symbol_meta_t meta;
        if (app_state_get_symbol_meta(i, &meta) != ESP_OK)
        {
            continue;
        }
        strncpy(cfg.symbols[i].ticker, meta.symbol, SETTINGS_SYMBOL_MAX_LEN);
        cfg.symbols[i].ticker[SETTINGS_SYMBOL_MAX_LEN] = '\0';
    }
    settings_store_save_symbols(&cfg);
}

// ctx points into the static s_watchlist_click_ctx array - see its
// declaration and s_wifi_click_ctx's comment for why a static (not
// stack-local) array is used here.
static void watchlist_remove_click_cb(lv_event_t *e)
{
    const watchlist_row_click_ctx_t *ctx = (const watchlist_row_click_ctx_t *)lv_event_get_user_data(e);
    if (app_state_remove_symbol(ctx->index) != ESP_OK)
    {
        return;
    }
    watchlist_save_current_symbols();
    watchlist_manage_rebuild();
}

// Arms a drag: captures start state from ctx and the current touch point,
// and disables s_watchlist_list's scrolling for the duration so the list's
// own scroll gesture can't compete with the drag (only this handle's small
// click area triggers a drag, so scrolling everywhere else is unaffected).
static void watchlist_drag_pressed_cb(lv_event_t *e)
{
    const watchlist_row_click_ctx_t *ctx = (const watchlist_row_click_ctx_t *)lv_event_get_user_data(e);
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    s_watchlist_drag.active = true;
    s_watchlist_drag.row = ctx->row;
    s_watchlist_drag.start_index = ctx->index;
    s_watchlist_drag.start_point_y = point.y;
    s_watchlist_drag.baseline_y = point.y;

    lv_obj_remove_flag(s_watchlist_list,
                        LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

// Fires continuously while the handle is held and the finger moves.
// lv_obj_move_to_index() below changes the row's own flex-computed base y
// (it now occupies a different slot), so the translate offset must be
// re-based on baseline_y - not on the fixed start_point_y - or the row
// visually jumps by one extra row height every time it crosses into a new
// slot. baseline_y is updated *before* the translate is applied so a
// crossing that happens on this very call is already accounted for.
static void watchlist_drag_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_watchlist_drag.active)
    {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    int32_t steps = (point.y - s_watchlist_drag.baseline_y) / WATCHLIST_ROW_HEIGHT_PX;
    if (steps != 0)
    {
        int32_t last_index = (int32_t)app_state_symbol_count() - 1; // excludes the trailing "Add symbol" pill
        int32_t current_index = lv_obj_get_index(s_watchlist_drag.row);
        int32_t candidate_index = current_index + steps;
        if (candidate_index < 0)
        {
            candidate_index = 0;
        }
        else if (candidate_index > last_index)
        {
            candidate_index = last_index;
        }
        if (candidate_index != current_index)
        {
            lv_obj_move_to_index(s_watchlist_drag.row, candidate_index);
        }
        s_watchlist_drag.baseline_y += steps * WATCHLIST_ROW_HEIGHT_PX;
    }

    lv_obj_set_style_translate_y(s_watchlist_drag.row, point.y - s_watchlist_drag.baseline_y, 0);
}

// Shared by LV_EVENT_RELEASED and LV_EVENT_PRESS_LOST - a lost touch (finger
// drags off the handle's click area or off-screen) must end the drag the
// same way a clean release does, or s_watchlist_list would be stuck
// non-scrollable. Persists the new order only if it actually changed, to
// avoid a needless NVS blob rewrite on a no-op drag.
static void watchlist_drag_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_watchlist_drag.active)
    {
        return;
    }

    lv_obj_set_style_translate_y(s_watchlist_drag.row, 0, 0);
    uint8_t start_index = s_watchlist_drag.start_index;
    uint8_t final_index = (uint8_t)lv_obj_get_index(s_watchlist_drag.row);

    lv_obj_add_flag(s_watchlist_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    s_watchlist_drag = (watchlist_drag_state_t){0};

    if (final_index == start_index)
    {
        return;
    }
    if (app_state_move_symbol(start_index, final_index) != ESP_OK)
    {
        return;
    }
    watchlist_save_current_symbols();
    watchlist_manage_rebuild(); // resyncs s_watchlist_click_ctx[], stale after move_to_index() during the drag
}

// One row per watchlist symbol: a drag handle, ticker (flex-grow), and a
// small red remove button. `index` selects this row's slot in
// s_watchlist_click_ctx - see that array's declaration and
// build_wifi_ap_row()'s analogous comment.
static void build_watchlist_symbol_row(const char *ticker, uint8_t index)
{
    lv_obj_t *row = lv_obj_create(s_watchlist_list);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    lv_obj_set_size(row, LV_PCT(100), WATCHLIST_ROW_HEIGHT_PX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 18, 0);
    lv_obj_set_style_pad_right(row, 18, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);

    watchlist_row_click_ctx_t *ctx = &s_watchlist_click_ctx[index];
    ctx->index = index;
    ctx->row = row;

    // Drag handle - the sole trigger for a reorder drag, deliberately kept
    // separate from the rest of the (non-clickable) row so the list's own
    // vertical scroll gesture is never in contention with it.
    lv_obj_t *drag_handle = lv_obj_create(row);
    lv_obj_remove_style_all(drag_handle);
    make_plain_container(drag_handle);
    // PRESS_LOCK back on top of make_plain_container()'s strip: without it,
    // lv_indev.c re-searches for the object under the finger on every move
    // once the point drifts off the handle's (small) hit area and sends
    // this handle a premature PRESS_LOST, aborting the drag after ~1 row.
    // PRESS_LOCK keeps this handle as indev's act_obj for the whole press
    // regardless of where the finger travels - exactly what a drag needs.
    lv_obj_add_flag(drag_handle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_size(drag_handle, 28, 28);
    lv_obj_set_flex_flow(drag_handle, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(drag_handle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(drag_handle, 8);
    lv_obj_add_event_cb(drag_handle, watchlist_drag_pressed_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(drag_handle, watchlist_drag_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(drag_handle, watchlist_drag_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(drag_handle, watchlist_drag_released_cb, LV_EVENT_PRESS_LOST, NULL);

    // Six-dot "grip" (2 columns x 3 rows), positioned by hand rather than via
    // flex-wrap - LVGL's built-in symbol set (lv_symbol_def.h) has no
    // dedicated drag-handle glyph, and this reads unambiguously as "drag"
    // where a hamburger/list icon (LV_SYMBOL_LIST) doesn't.
    static const int8_t k_drag_dot_pos[6][2] = {
        {0, 0}, {7, 0}, {0, 6}, {7, 6}, {0, 12}, {7, 12},
    };
    // Both the dot grid and each dot must stay non-clickable/non-scrollable
    // (make_plain_container(), same as every other decorative sub-element in
    // this file) - otherwise a tap could hit a dot instead of drag_handle,
    // missing the PRESSED/PRESSING/RELEASED handlers registered on it.
    lv_obj_t *drag_dots = lv_obj_create(drag_handle);
    lv_obj_remove_style_all(drag_dots);
    make_plain_container(drag_dots);
    lv_obj_set_size(drag_dots, 11, 16);
    for (uint8_t i = 0; i < 6; i++)
    {
        lv_obj_t *dot = lv_obj_create(drag_dots);
        lv_obj_remove_style_all(dot);
        make_plain_container(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_pos(dot, k_drag_dot_pos[i][0], k_drag_dot_pos[i][1]);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, COLOR_MUTED_DIM, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    lv_obj_t *ticker_label = lv_label_create(row);
    lv_obj_set_flex_grow(ticker_label, 1);
    lv_obj_set_style_text_color(ticker_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(ticker_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(ticker_label, ticker);

    lv_obj_t *remove_btn = lv_button_create(row);
    lv_obj_remove_style_all(remove_btn);
    make_plain_container(remove_btn);
    lv_obj_add_flag(remove_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(remove_btn, 28, 28);
    lv_obj_set_style_radius(remove_btn, 8, 0);
    lv_obj_set_style_bg_color(remove_btn, lv_color_hex(0x241318), 0);
    lv_obj_set_style_bg_opa(remove_btn, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(remove_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(remove_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(remove_btn, 8);
    lv_obj_add_event_cb(remove_btn, watchlist_remove_click_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *remove_icon = lv_label_create(remove_btn);
    lv_obj_set_style_text_color(remove_icon, COLOR_DOWN, 0);
    lv_label_set_text(remove_icon, LV_SYMBOL_TRASH);
}

static void watchlist_add_row_click_cb(lv_event_t *e)
{
    (void)e;
    watchlist_add_screen_reset();
    show_settings_view(SETTINGS_VIEW_WATCHLIST_ADD);
}

// "+ Add symbol" entry point, modeled on build_wifi_add_network_row()'s
// bordered pill. Disabled (dimmed, non-clickable) at the watchlist cap per
// docs/decisions/0007-watchlist-management.md: display_ui must gate this
// itself rather than rely solely on app_state_add_symbol()'s ESP_ERR_NO_MEM.
static void build_watchlist_add_row(lv_obj_t *parent, bool enabled)
{
    const int32_t addbtn_width_px = BOARD_JC4880P443C_LCD_H_RES - 2 * 18;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    make_plain_container(row);
    if (enabled)
    {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, watchlist_add_row_click_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_set_size(row, addbtn_width_px, 46);
    lv_obj_set_style_margin_left(row, 18, 0);
    lv_obj_set_style_margin_top(row, 12, 0);
    lv_obj_set_style_margin_bottom(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_HAIRLINE, 0);
    lv_obj_set_style_opa(row, enabled ? LV_OPA_COVER : LV_OPA_50, 0);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_style_text_color(icon, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
    lv_label_set_text(icon, LV_SYMBOL_PLUS);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_label_set_text(label, "Add symbol");
}

// Tears down and rebuilds the symbol list + "Add symbol" row from
// app_state's current watchlist. Called whenever the Manage screen is
// entered and after every add/remove - this is a Settings action, not a
// per-tick poll, so (unlike update_wifi_screen()) an unconditional rebuild
// on every call is fine; nothing here runs concurrently with a tap on one
// of these rows.
static void watchlist_manage_rebuild(void)
{
    uint8_t count = app_state_symbol_count();
    if (count > SETTINGS_MAX_WATCHLIST)
    {
        count = SETTINGS_MAX_WATCHLIST;
    }
    lv_label_set_text_fmt(s_watchlist_manage_subtitle, "%u of %u", (unsigned)count, (unsigned)SETTINGS_MAX_WATCHLIST);

    lv_obj_clean(s_watchlist_list);
    for (uint8_t i = 0; i < count; i++)
    {
        app_state_symbol_meta_t meta;
        if (app_state_get_symbol_meta(i, &meta) != ESP_OK)
        {
            continue;
        }
        build_watchlist_symbol_row(meta.symbol, i);
    }
    build_watchlist_add_row(s_watchlist_list, count < SETTINGS_MAX_WATCHLIST);
}

static void watchlist_manage_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LIST);
}

static void build_watchlist_manage_screen(lv_obj_t *screen)
{
    s_watchlist_manage_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_watchlist_manage_screen);
    make_plain_container(s_watchlist_manage_screen);
    lv_obj_set_size(s_watchlist_manage_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_watchlist_manage_screen, LV_FLEX_FLOW_COLUMN);

    s_watchlist_manage_subtitle =
        build_subscreen_header(s_watchlist_manage_screen, "Watchlist symbols", "--", watchlist_manage_back_cb);

    s_watchlist_list = lv_obj_create(s_watchlist_manage_screen);
    lv_obj_remove_style_all(s_watchlist_list);
    // Genuinely scrollable (up to SETTINGS_MAX_WATCHLIST rows plus the "Add
    // symbol" pill can run longer than the screen) - same flag treatment as
    // s_wifi_list, see its comment.
    lv_obj_remove_flag(s_watchlist_list, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_PRESS_LOCK |
                                              LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_width(s_watchlist_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_watchlist_list, 1);
    lv_obj_set_flex_flow(s_watchlist_list, LV_FLEX_FLOW_COLUMN);
}

// --- Add symbol screen ---

// The exact same keyboard as the Wi-Fi password screen (lowercase/uppercase
// letters + two symbol pages, full shift/mode-switch set) rather than a
// restricted ticker-only layout - the only difference from
// s_wifi_kb_map_lc/uc/sym_1/sym_2 is the accent action key ("Search"
// instead of "Connect"). Whatever the user actually types gets sanitized
// (stripped of non-alphanumerics, upper-cased) in watchlist_add_check_cb()
// before it's sent as a symbol, so allowing the full keyboard here (spaces,
// punctuation, lowercase) is harmless - same reasoning as letting the Wi-Fi
// SSID field accept anything and validating server-side.

// Custom mode IDs beyond LVGL's built-in TEXT_LOWER/TEXT_UPPER, used with
// lv_keyboard_set_mode() below - same technique as WIFI_KB_MODE_SYM_1/2, but
// with distinct USER_3/4 values. lv_keyboard_set_map() writes into LVGL's
// shared global mode->map table (see the comment by WIFI_KB_MODE_SYM_1/2), so
// reusing IDs 2/3 here used to let this screen's build (which runs after
// build_wifi_password_screen()) silently overwrite the Wi-Fi keyboard's
// symbol-page map with this one's - "Search" would render, and get typed
// literally, when the Wi-Fi "Add Network" keyboard switched to symbols.
#define WATCHLIST_KB_MODE_SYM_1 LV_KEYBOARD_MODE_USER_3
#define WATCHLIST_KB_MODE_SYM_2 LV_KEYBOARD_MODE_USER_4

static const char *const s_watchlist_kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "123", " ", "Search", "",
};

static const char *const s_watchlist_kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "123", " ", "Search", "",
};

static const char *const s_watchlist_kb_map_sym_1[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "_", ".", ",", ":", ";", "@", "(", ")", "\n",
    "#+=", "'", "\"", "!", "?", "*", LV_SYMBOL_BACKSPACE, "\n",
    "abc", " ", "Search", "",
};

static const char *const s_watchlist_kb_map_sym_2[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "/", "\\", "+", "=", "<", ">", "#", "[", "]", "\n",
    "!?*", "{", "}", "%", "&", "$", LV_SYMBOL_BACKSPACE, "\n",
    "abc", " ", "Search", "",
};

static const lv_buttonmatrix_ctrl_t s_watchlist_kb_ctrl_map[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // row 1: 10 keys
    1, 1, 1, 1, 1, 1, 1, 1, 1,    // row 2: 9 keys
    2, 1, 1, 1, 1, 1, 1, 1, 2,    // row 3: shift (2x), 7 keys (1x), backspace (2x)
    2, 5, 3, 1,                  // row 4: mode-switch (2x), space (5x), Check symbol (3x), padding
};

static const lv_buttonmatrix_ctrl_t s_watchlist_kb_ctrl_map_sym[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // row 1: 10 keys
    1, 1, 1, 1, 1, 1, 1, 1, 1,    // row 2: 9 keys
    2, 1, 1, 1, 1, 1, 2,          // row 3: mode-switch (2x), 5 keys (1x), backspace (2x)
    2, 5, 3, 1,                  // row 4: "abc" (2x), space (5x), Check symbol (3x), padding
};

static void watchlist_add_check_cb(lv_event_t *e); // defined below, referenced by the keyboard event cb

// Same intercept-and-delegate shape as wifi_keyboard_event_cb(): owns every
// mode-switch button directly, delegates letters/space/backspace to the
// default handler.
static void watchlist_add_keyboard_event_cb(lv_event_t *e)
{
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

    if (strcmp(txt, "Search") == 0)
    {
        watchlist_add_check_cb(e);
        return;
    }
    // See the matching comment in wifi_keyboard_event_cb(): lv_keyboard's
    // per-mode maps live in one global table shared by every lv_keyboard in
    // the app, so each branch re-asserts this keyboard's own map/ctrl-map
    // right after lv_keyboard_set_mode() instead of trusting it - otherwise
    // this keyboard (built after the Wi-Fi one, sharing its TEXT_LOWER/UPPER
    // modes) would keep winning that table and the Wi-Fi keyboard would show
    // "Search" instead of "Connect" after any case switch.
    if (strcmp(txt, "ABC") == 0)
    {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
        lv_buttonmatrix_set_map(kb, s_watchlist_kb_map_uc);
        lv_buttonmatrix_set_ctrl_map(kb, s_watchlist_kb_ctrl_map);
        return;
    }
    if (strcmp(txt, "abc") == 0)
    {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_buttonmatrix_set_map(kb, s_watchlist_kb_map_lc);
        lv_buttonmatrix_set_ctrl_map(kb, s_watchlist_kb_ctrl_map);
        return;
    }
    if (strcmp(txt, "123") == 0 || strcmp(txt, "!?*") == 0)
    {
        lv_keyboard_set_mode(kb, WATCHLIST_KB_MODE_SYM_1);
        lv_buttonmatrix_set_map(kb, s_watchlist_kb_map_sym_1);
        lv_buttonmatrix_set_ctrl_map(kb, s_watchlist_kb_ctrl_map_sym);
        return;
    }
    if (strcmp(txt, "#+=") == 0)
    {
        lv_keyboard_set_mode(kb, WATCHLIST_KB_MODE_SYM_2);
        lv_buttonmatrix_set_map(kb, s_watchlist_kb_map_sym_2);
        lv_buttonmatrix_set_ctrl_map(kb, s_watchlist_kb_ctrl_map_sym);
        return;
    }

    lv_keyboard_def_event_cb(e);
}

// Hides both result states (match card / error note) and the "Add to
// watchlist" button - the screen's starting state, and the state after any
// edit to the field following a check (mockup doesn't keep a stale result
// visible once the user starts retyping... but per the reviewed design,
// the keyboard/result actually *does* stay up across repeated checks; this
// is only used on screen entry/reset, not after every keystroke).
static void watchlist_add_hide_result(void)
{
    lv_obj_add_flag(s_watchlist_match_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_watchlist_error_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_watchlist_add_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_watchlist_status_label, LV_OBJ_FLAG_HIDDEN);
    s_watchlist_match_valid = false;
}

// Fires on every edit to the symbol field (typing, backspace, paste) -
// a match/error from a previously-checked symbol shouldn't linger once the
// user starts changing what they typed. Also fires for the reset's own
// lv_textarea_set_text(""), which is harmless (nothing to hide yet there).
static void watchlist_symbol_input_changed_cb(lv_event_t *e)
{
    (void)e;
    watchlist_add_hide_result();
}

// Resets the screen to its empty, pre-check state - called every time this
// screen is entered so a stale match/error from a previous symbol never
// carries over.
static void watchlist_add_screen_reset(void)
{
    lv_textarea_set_text(s_watchlist_symbol_input, "");
    watchlist_add_hide_result();

    uint8_t count = app_state_symbol_count();
    if (count > SETTINGS_MAX_WATCHLIST)
    {
        count = SETTINGS_MAX_WATCHLIST;
    }
    lv_label_set_text_fmt(s_watchlist_add_subtitle, "%u of %u used", (unsigned)count, (unsigned)SETTINGS_MAX_WATCHLIST);
}

// Strips everything but letters/digits from raw and upper-cases the rest
// into out (capacity out_cap, always NUL-terminated) - so stray spaces,
// punctuation, or lowercase entered on the full keyboard above still
// produce a well-formed Binance pair (e.g. "ltc usdt!" -> "LTCUSDT").
// Returns the sanitized length.
static size_t watchlist_sanitize_symbol(const char *raw, char *out, size_t out_cap)
{
    size_t n = 0;
    for (const char *p = raw; *p != '\0' && n + 1 < out_cap; p++)
    {
        if (isalnum((unsigned char)*p))
        {
            out[n++] = (char)toupper((unsigned char)*p);
        }
    }
    out[n] = '\0';
    return n;
}

// Fires from the keyboard's "Search" key. Blocking, like every other
// market_data_client call - this runs synchronously on the LVGL task for
// the ~0.5-2s HTTPS round trip (see docs/decisions on this screen's scope:
// no background-task/queue scaffolding for this slice, matching every
// existing market_data_client call site). A "Searching..." label plus an
// explicit lv_refr_now() flushes that state to the display before the
// blocking call, since LVGL otherwise wouldn't redraw until this callback
// returns.
static void watchlist_add_check_cb(lv_event_t *e)
{
    (void)e;
    char symbol[SETTINGS_SYMBOL_MAX_LEN + 1];
    if (watchlist_sanitize_symbol(lv_textarea_get_text(s_watchlist_symbol_input), symbol, sizeof(symbol)) == 0)
    {
        return;
    }

    lv_obj_add_flag(s_watchlist_match_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_watchlist_error_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_watchlist_add_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_watchlist_status_label, COLOR_MUTED, 0);
    lv_label_set_text(s_watchlist_status_label, "Searching...");
    lv_obj_remove_flag(s_watchlist_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);

    market_data_ticker_24hr_t ticker;
    market_data_err_t err = market_data_client_fetch_ticker_24hr(symbol, &ticker);
    lv_obj_add_flag(s_watchlist_status_label, LV_OBJ_FLAG_HIDDEN);

    if (err == MARKET_DATA_OK)
    {
        strncpy(s_watchlist_pending_symbol, symbol, SETTINGS_SYMBOL_MAX_LEN);
        s_watchlist_pending_symbol[SETTINGS_SYMBOL_MAX_LEN] = '\0';
        s_watchlist_match_valid = true;

        lv_label_set_text(s_watchlist_match_pair_label, s_watchlist_pending_symbol);

        char price_buf[24];
        char hi_buf[24];
        char lo_buf[24];
        // Same "L <lo>\nH <hi>" style as the watchlist row - see the comment
        // at that call site in update_row().
        char range_buf[2 * sizeof(hi_buf) + 8];
        char change_buf[16];
        display_format_price(ticker.last_price, price_buf, sizeof(price_buf));
        display_format_price(ticker.high_price, hi_buf, sizeof(hi_buf));
        display_format_price(ticker.low_price, lo_buf, sizeof(lo_buf));
        snprintf(range_buf, sizeof(range_buf), "L %s\nH %s", lo_buf, hi_buf);
        snprintf(change_buf, sizeof(change_buf), "%+.2f%%", ticker.price_change_percent);

        lv_label_set_text(s_watchlist_match_last_price_label, price_buf);
        lv_obj_set_style_text_color(s_watchlist_match_change_label,
                                     ticker.price_change_percent >= 0.0 ? COLOR_UP : COLOR_DOWN, 0);
        lv_label_set_text(s_watchlist_match_change_label, change_buf);
        lv_label_set_text(s_watchlist_match_range_label, range_buf);

        lv_obj_remove_flag(s_watchlist_match_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_watchlist_add_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_watchlist_error_note, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        s_watchlist_match_valid = false;
        lv_obj_add_flag(s_watchlist_match_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_watchlist_add_button, LV_OBJ_FLAG_HIDDEN);

        const char *msg = (err == MARKET_DATA_ERR_SYMBOL_NOT_FOUND)
                               ? "isn't a Binance spot pair. Check the spelling."
                               : "Couldn't reach Binance. Try again.";
        lv_label_set_text_fmt(s_watchlist_error_label, "%s %s", symbol, msg);
        lv_obj_remove_flag(s_watchlist_error_note, LV_OBJ_FLAG_HIDDEN);
    }
}

// Fires from the "Add to watchlist" button, shown only after a successful
// check. Persists the symbol that was actually validated
// (s_watchlist_pending_symbol), not necessarily whatever text is currently
// in the field.
static void watchlist_add_to_watchlist_cb(lv_event_t *e)
{
    (void)e;
    if (!s_watchlist_match_valid)
    {
        return;
    }
    if (app_state_add_symbol(s_watchlist_pending_symbol) != ESP_OK)
    {
        return;
    }
    watchlist_save_current_symbols();
    watchlist_manage_rebuild();
    show_settings_view(SETTINGS_VIEW_WATCHLIST_MANAGE);
}

static void watchlist_add_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_WATCHLIST_MANAGE);
}

// One "key: value" column, used three times in the match card (last price,
// 24h change, 24h range). Returns the value label so the caller can update it.
static lv_obj_t *build_watchlist_stat(lv_obj_t *parent, const char *key_text)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    make_plain_container(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 3, 0);

    lv_obj_t *key_label = lv_label_create(col);
    lv_obj_set_style_text_color(key_label, lv_color_hex(0x5E8F73), 0); // muted green, matches the match-card's tint
    lv_obj_set_style_text_font(key_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(key_label, key_text);

    lv_obj_t *value_label = lv_label_create(col);
    lv_obj_set_style_text_color(value_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_14, 0);
    // Overflow safety net: a micro-cap symbol's 8-decimal "hi / lo" text can
    // outrun this column - truncate with "..." instead of overlapping siblings.
    lv_obj_set_width(value_label, WATCHLIST_MATCH_STAT_VALUE_WIDTH_PX);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(value_label, "--");
    return value_label;
}

// Same "key: value" column shape as build_watchlist_stat(), but with a
// neutral muted key label instead of that helper's hardcoded green tint -
// the Updates status card's background color changes per-state (green/blue/
// neutral - see updates_screen_reset()), so a fixed green key label would
// clash against the non-green states.
static lv_obj_t *build_updates_stat(lv_obj_t *parent, const char *key_text)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    make_plain_container(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 3, 0);

    lv_obj_t *key_label = lv_label_create(col);
    lv_obj_set_style_text_color(key_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(key_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(key_label, key_text);

    lv_obj_t *value_label = lv_label_create(col);
    lv_obj_set_style_text_color(value_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(value_label, "--");
    return value_label;
}

static void build_watchlist_add_screen(lv_obj_t *screen)
{
    s_watchlist_add_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_watchlist_add_screen);
    make_plain_container(s_watchlist_add_screen);
    lv_obj_set_size(s_watchlist_add_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_watchlist_add_screen, LV_FLEX_FLOW_COLUMN);

    s_watchlist_add_subtitle = build_subscreen_header(s_watchlist_add_screen, "Add symbol", "--", watchlist_add_back_cb);

    const int32_t field_row_width_px = (int32_t)(BOARD_JC4880P443C_LCD_H_RES * 9 / 10) + 20;
    const int32_t field_vpad_px = (WIFI_PASSWORD_FIELD_HEIGHT_PX - lv_font_get_line_height(&lv_font_montserrat_16)) / 2;

    // Everything above the keyboard sits in one flex_grow:1 wrapper, so it
    // fills the leftover space and the keyboard stays pinned to the bottom
    // regardless of whether the match card/error note is showing - same
    // technique as build_wifi_password_screen()'s fields_wrap.
    lv_obj_t *content_wrap = lv_obj_create(s_watchlist_add_screen);
    lv_obj_remove_style_all(content_wrap);
    make_plain_container(content_wrap);
    lv_obj_set_size(content_wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(content_wrap, 1);
    lv_obj_set_flex_flow(content_wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_wrap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *input_label = lv_label_create(content_wrap);
    lv_obj_set_style_text_color(input_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(input_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(input_label, 1, 0);
    lv_obj_set_style_pad_left(input_label, 20, 0);
    lv_obj_set_style_pad_top(input_label, 14, 0);
    lv_label_set_text(input_label, "BINANCE PAIR");

    lv_obj_t *field_wrap = lv_obj_create(content_wrap);
    lv_obj_remove_style_all(field_wrap);
    make_plain_container(field_wrap);
    lv_obj_set_size(field_wrap, field_row_width_px, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(field_wrap, 20, 0);
    lv_obj_set_style_margin_top(field_wrap, 8, 0);

    s_watchlist_symbol_input = lv_textarea_create(field_wrap);
    style_dark_textarea(s_watchlist_symbol_input);
    lv_textarea_set_one_line(s_watchlist_symbol_input, true);
    lv_textarea_set_max_length(s_watchlist_symbol_input, SETTINGS_SYMBOL_MAX_LEN);
    lv_obj_set_width(s_watchlist_symbol_input, LV_PCT(100));
    lv_obj_set_height(s_watchlist_symbol_input, WIFI_PASSWORD_FIELD_HEIGHT_PX);
    lv_obj_set_style_pad_top(s_watchlist_symbol_input, field_vpad_px, 0);
    lv_obj_set_style_pad_bottom(s_watchlist_symbol_input, field_vpad_px, 0);
    lv_obj_set_style_border_width(s_watchlist_symbol_input, 0, 0);
    lv_obj_add_event_cb(s_watchlist_symbol_input, watchlist_symbol_input_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *hint_label = lv_label_create(content_wrap);
    lv_obj_set_style_text_color(hint_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(hint_label, field_row_width_px);
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_margin_left(hint_label, 20, 0);
    lv_obj_set_style_pad_top(hint_label, 8, 0);
    lv_label_set_text(hint_label, "Type the exact pair, e.g. LTCUSDT - not a name search.");

    // "Searching..." indicator - only visible while watchlist_add_check_cb()'s
    // blocking market_data_client call is in flight.
    s_watchlist_status_label = lv_label_create(content_wrap);
    lv_obj_set_style_text_font(s_watchlist_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_margin_left(s_watchlist_status_label, 20, 0);
    lv_obj_set_style_pad_top(s_watchlist_status_label, 10, 0);
    lv_label_set_text(s_watchlist_status_label, "");

    // Match card: populated and shown only after a successful "Search".
    s_watchlist_match_card = lv_obj_create(content_wrap);
    lv_obj_remove_style_all(s_watchlist_match_card);
    make_plain_container(s_watchlist_match_card);
    lv_obj_set_size(s_watchlist_match_card, field_row_width_px, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(s_watchlist_match_card, 20, 0);
    lv_obj_set_style_margin_top(s_watchlist_match_card, 14, 0);
    lv_obj_set_style_pad_all(s_watchlist_match_card, 14, 0);
    lv_obj_set_style_radius(s_watchlist_match_card, 12, 0);
    lv_obj_set_style_bg_color(s_watchlist_match_card, lv_color_hex(0x0F2A1E), 0);
    lv_obj_set_style_bg_opa(s_watchlist_match_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_watchlist_match_card, 1, 0);
    lv_obj_set_style_border_color(s_watchlist_match_card, lv_color_hex(0x1E4430), 0);
    lv_obj_set_flex_flow(s_watchlist_match_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_watchlist_match_card, 8, 0);

    s_watchlist_match_pair_label = lv_label_create(s_watchlist_match_card);
    lv_obj_set_style_text_color(s_watchlist_match_pair_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_watchlist_match_pair_label, &lv_font_montserrat_18, 0);

    lv_obj_t *avail_row = lv_obj_create(s_watchlist_match_card);
    lv_obj_remove_style_all(avail_row);
    make_plain_container(avail_row);
    lv_obj_set_size(avail_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(avail_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(avail_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(avail_row, 6, 0);

    lv_obj_t *avail_icon = lv_label_create(avail_row);
    lv_obj_set_style_text_color(avail_icon, COLOR_UP, 0);
    lv_label_set_text(avail_icon, LV_SYMBOL_OK);

    lv_obj_t *avail_label = lv_label_create(avail_row);
    lv_obj_set_style_text_color(avail_label, COLOR_UP, 0);
    lv_obj_set_style_text_font(avail_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(avail_label, "Found on Binance");

    lv_obj_t *stats_row = lv_obj_create(s_watchlist_match_card);
    lv_obj_remove_style_all(stats_row);
    make_plain_container(stats_row);
    lv_obj_set_size(stats_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_watchlist_match_last_price_label = build_watchlist_stat(stats_row, "LAST PRICE");
    s_watchlist_match_change_label = build_watchlist_stat(stats_row, "24H CHANGE");
    s_watchlist_match_range_label = build_watchlist_stat(stats_row, "24H HIGH / LOW");

    // Error note: shown only after a failed "Search".
    s_watchlist_error_note = lv_obj_create(content_wrap);
    lv_obj_remove_style_all(s_watchlist_error_note);
    make_plain_container(s_watchlist_error_note);
    lv_obj_set_size(s_watchlist_error_note, field_row_width_px, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(s_watchlist_error_note, 20, 0);
    lv_obj_set_style_margin_top(s_watchlist_error_note, 14, 0);
    lv_obj_set_style_pad_all(s_watchlist_error_note, 12, 0);
    lv_obj_set_style_radius(s_watchlist_error_note, 10, 0);
    lv_obj_set_style_bg_color(s_watchlist_error_note, lv_color_hex(0x241318), 0);
    lv_obj_set_style_bg_opa(s_watchlist_error_note, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_watchlist_error_note, 1, 0);
    lv_obj_set_style_border_color(s_watchlist_error_note, lv_color_hex(0x3A1B22), 0);

    s_watchlist_error_label = lv_label_create(s_watchlist_error_note);
    lv_obj_set_style_text_color(s_watchlist_error_label, lv_color_hex(0xFF8B95), 0);
    lv_obj_set_style_text_font(s_watchlist_error_label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_watchlist_error_label, LV_PCT(100));
    lv_label_set_long_mode(s_watchlist_error_label, LV_LABEL_LONG_WRAP);

    // "Add to watchlist": shown only after a successful check.
    s_watchlist_add_button = lv_button_create(content_wrap);
    lv_obj_remove_style_all(s_watchlist_add_button);
    make_plain_container(s_watchlist_add_button);
    lv_obj_add_flag(s_watchlist_add_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_watchlist_add_button, field_row_width_px, 44);
    lv_obj_set_style_margin_left(s_watchlist_add_button, 20, 0);
    lv_obj_set_style_margin_top(s_watchlist_add_button, 14, 0);
    lv_obj_set_style_radius(s_watchlist_add_button, 9, 0);
    lv_obj_set_style_bg_color(s_watchlist_add_button, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_watchlist_add_button, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_watchlist_add_button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_watchlist_add_button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_watchlist_add_button, watchlist_add_to_watchlist_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *add_button_label = lv_label_create(s_watchlist_add_button);
    lv_obj_set_style_text_color(add_button_label, lv_color_hex(0x04141C), 0);
    lv_obj_set_style_text_font(add_button_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(add_button_label, "Add to watchlist");

    watchlist_add_hide_result(); // starts with neither result state visible

    s_watchlist_add_keyboard = lv_keyboard_create(s_watchlist_add_screen);
    style_dark_keyboard(s_watchlist_add_keyboard);
    lv_obj_set_height(s_watchlist_add_keyboard, (BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX) / 2 - 50);
    lv_keyboard_set_map(s_watchlist_add_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, s_watchlist_kb_map_lc,
                         s_watchlist_kb_ctrl_map);
    lv_keyboard_set_map(s_watchlist_add_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, s_watchlist_kb_map_uc,
                         s_watchlist_kb_ctrl_map);
    lv_keyboard_set_map(s_watchlist_add_keyboard, WATCHLIST_KB_MODE_SYM_1, s_watchlist_kb_map_sym_1,
                         s_watchlist_kb_ctrl_map_sym);
    lv_keyboard_set_map(s_watchlist_add_keyboard, WATCHLIST_KB_MODE_SYM_2, s_watchlist_kb_map_sym_2,
                         s_watchlist_kb_ctrl_map_sym);
    lv_keyboard_set_mode(s_watchlist_add_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_watchlist_add_keyboard, s_watchlist_symbol_input);
    lv_obj_remove_event_cb(s_watchlist_add_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_watchlist_add_keyboard, watchlist_add_keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// --- Updates screen ---

static void updates_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LIST);
}

// Re-reads current/latest version and last-checked time into the screen's
// labels, restyles the status card for the current state, and resets
// button/status/progress visibility to match app_state's current OTA info -
// called on screen-entry and again after a successful manual check, same
// screen-entry-reset convention as watchlist_add_screen_reset().
static void updates_screen_reset(void)
{
    const esp_app_desc_t *running = esp_app_get_description();
    lv_label_set_text(s_updates_current_version_label, running->version);

    app_state_ota_info_t info;
    app_state_get_ota_info(&info);
    lv_label_set_text(s_updates_latest_version_label, info.latest_version[0] != '\0' ? info.latest_version : "--");

    char relative_buf[24];
    format_relative_time(info.last_check_ms, relative_buf, sizeof(relative_buf));
    lv_label_set_text_fmt(s_updates_last_checked_label, "Last checked: %s", relative_buf);

    if (info.latest_version[0] == '\0')
    {
        // Never checked yet - neutral, not a false "up to date" claim (same
        // reasoning as update_statusbar()'s Settings-row description).
        lv_obj_set_style_bg_color(s_updates_state_card, COLOR_HAIRLINE, 0);
        lv_obj_set_style_border_color(s_updates_state_card, COLOR_HAIRLINE, 0);
        lv_obj_set_style_text_color(s_updates_state_icon, COLOR_MUTED, 0);
        lv_label_set_text(s_updates_state_icon, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_color(s_updates_state_text, COLOR_MUTED, 0);
        lv_label_set_text(s_updates_state_text, "Not checked yet");
    }
    else if (info.update_available)
    {
        lv_obj_set_style_bg_color(s_updates_state_card, lv_color_hex(0x0F232A), 0);
        lv_obj_set_style_border_color(s_updates_state_card, lv_color_hex(0x1E3A46), 0);
        lv_obj_set_style_text_color(s_updates_state_icon, COLOR_ACCENT, 0);
        lv_label_set_text(s_updates_state_icon, LV_SYMBOL_DOWNLOAD);
        lv_obj_set_style_text_color(s_updates_state_text, COLOR_ACCENT, 0);
        lv_label_set_text(s_updates_state_text, "Update available");
    }
    else
    {
        lv_obj_set_style_bg_color(s_updates_state_card, lv_color_hex(0x0F2A1E), 0);
        lv_obj_set_style_border_color(s_updates_state_card, lv_color_hex(0x1E4430), 0);
        lv_obj_set_style_text_color(s_updates_state_icon, COLOR_UP, 0);
        lv_label_set_text(s_updates_state_icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(s_updates_state_text, COLOR_UP, 0);
        lv_label_set_text(s_updates_state_text, "Up to date");
    }

    lv_obj_add_flag(s_updates_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_updates_progress_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_updates_action_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(s_updates_action_button, LV_STATE_DISABLED);
    lv_label_set_text(s_updates_action_label,
                       info.update_available ? LV_SYMBOL_DOWNLOAD " Install update" : LV_SYMBOL_REFRESH
                                                                                       " Check for updates");
}

static void updates_row_click_cb(lv_event_t *e)
{
    (void)e;
    updates_screen_reset();
    show_settings_view(SETTINGS_VIEW_UPDATES);
}

// ota_client_update_to()'s progress callback - fires repeatedly during the
// download, on the same task/context that called it (the LVGL task, from
// updates_action_cb() below), so it's safe to touch UI objects directly.
// Skips the redraw when the rounded percentage hasn't moved since the last
// call, since this can fire once per HTTP read (every
// OTA_CLIENT_HTTP_BUFFER_SIZE bytes) and forcing a full LVGL redraw that
// often would be wasted work for no visible change.
static void updates_install_progress_cb(size_t bytes_read, size_t total_bytes, void *ctx)
{
    (void)ctx;
    static int s_last_percent_shown = -1;

    int percent = (total_bytes > 0) ? (int)((bytes_read * 100) / total_bytes) : 0;
    if (percent > 100)
    {
        percent = 100;
    }
    if (percent == s_last_percent_shown)
    {
        return;
    }
    s_last_percent_shown = percent;

    lv_bar_set_value(s_updates_progress_bar, percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_updates_progress_label, "%d%%", percent);
    lv_refr_now(NULL);
}

// The one Updates action button: "Check for updates" when no update is
// known to be available, "Install update" once one is (label text set by
// updates_screen_reset()). Both branches are blocking, like every other
// user-triggered network call in this file (see watchlist_add_check_cb()'s
// comment) - bypassing app_state_ota_task's request-flag mechanism entirely
// since that task's loop only wakes every 60s, which would leave a tapped
// button looking dead for up to a minute. Installing has no separate
// confirm step - the button already reads "Install update" by the time a
// tap can reach this branch, so the tap itself is the explicit decision.
static void updates_action_cb(lv_event_t *e)
{
    (void)e;
    app_state_ota_info_t info;
    app_state_get_ota_info(&info);

    if (info.update_available)
    {
        lv_obj_add_flag(s_updates_action_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_updates_status_label, COLOR_MUTED, 0);
        lv_label_set_text(s_updates_status_label, "Installing update - do not power off...");
        lv_obj_remove_flag(s_updates_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_updates_progress_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_updates_progress_label, "0%");
        lv_obj_remove_flag(s_updates_progress_row, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);

        // Blocking - downloads+flashes the release and reboots the device
        // on success (never returns); only reached again here on failure.
        // ~24s for a ~1.66MB image per the OTA hardware-test doc.
        ota_client_err_t err = ota_client_update_to(info.latest_version, updates_install_progress_cb, NULL);

        lv_obj_add_flag(s_updates_progress_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_updates_status_label, COLOR_WARN, 0);
        lv_label_set_text(s_updates_status_label, ota_err_message(err));
        lv_obj_clear_state(s_updates_action_button, LV_STATE_DISABLED);
        lv_obj_remove_flag(s_updates_action_button, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_state(s_updates_action_button, LV_STATE_DISABLED);
    lv_label_set_text(s_updates_action_label, "Checking...");
    lv_obj_add_flag(s_updates_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);

    ota_client_release_info_t check_info;
    ota_client_err_t err = ota_client_check_latest_release(&check_info);

    if (err == OTA_CLIENT_OK)
    {
        app_state_set_ota_info(check_info.update_available, check_info.tag_name);
        updates_screen_reset(); // refreshes labels/card/button text and re-enables the button
    }
    else
    {
        lv_label_set_text(s_updates_action_label, LV_SYMBOL_REFRESH " Check for updates");
        lv_obj_clear_state(s_updates_action_button, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(s_updates_status_label, COLOR_WARN, 0);
        lv_label_set_text(s_updates_status_label, ota_err_message(err));
        lv_obj_remove_flag(s_updates_status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_updates_screen(lv_obj_t *screen)
{
    s_updates_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_updates_screen);
    make_plain_container(s_updates_screen);
    lv_obj_set_size(s_updates_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_updates_screen, LV_FLEX_FLOW_COLUMN);

    build_subscreen_header(s_updates_screen, "Updates", NULL, updates_back_cb);

    lv_obj_t *content = lv_obj_create(s_updates_screen);
    lv_obj_remove_style_all(content);
    make_plain_container(content);
    lv_obj_set_size(content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(content, 20, 0);
    lv_obj_set_style_pad_right(content, 20, 0);
    lv_obj_set_style_pad_top(content, 16, 0);
    lv_obj_set_style_pad_row(content, 14, 0);

    const int32_t button_width_px = BOARD_JC4880P443C_LCD_H_RES - 40; // matches content's 20px side padding

    // Status card: a colored icon+text badge (state-dependent - green/blue/
    // neutral, see updates_screen_reset()) plus current/latest version, all
    // in one rounded tinted card - same technique as the Watchlist Add
    // screen's match card (s_watchlist_match_card), just restyled per-state
    // here instead of a fixed green.
    s_updates_state_card = lv_obj_create(content);
    lv_obj_remove_style_all(s_updates_state_card);
    make_plain_container(s_updates_state_card);
    lv_obj_set_size(s_updates_state_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(s_updates_state_card, 14, 0);
    lv_obj_set_style_radius(s_updates_state_card, 12, 0);
    lv_obj_set_style_bg_opa(s_updates_state_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_updates_state_card, 1, 0);
    lv_obj_set_flex_flow(s_updates_state_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_updates_state_card, 12, 0);

    lv_obj_t *state_row = lv_obj_create(s_updates_state_card);
    lv_obj_remove_style_all(state_row);
    make_plain_container(state_row);
    lv_obj_set_size(state_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(state_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state_row, 8, 0);

    s_updates_state_icon = lv_label_create(state_row);
    lv_obj_set_style_text_font(s_updates_state_icon, &lv_font_montserrat_18, 0);

    s_updates_state_text = lv_label_create(state_row);
    lv_obj_set_style_text_font(s_updates_state_text, &lv_font_montserrat_16, 0);

    lv_obj_t *stats_row = lv_obj_create(s_updates_state_card);
    lv_obj_remove_style_all(stats_row);
    make_plain_container(stats_row);
    lv_obj_set_size(stats_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_updates_current_version_label = build_updates_stat(stats_row, "CURRENT VERSION");
    s_updates_latest_version_label = build_updates_stat(stats_row, "LATEST VERSION");

    s_updates_last_checked_label = lv_label_create(content);
    lv_obj_set_style_text_color(s_updates_last_checked_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_updates_last_checked_label, &lv_font_montserrat_12, 0);

    s_updates_status_label = lv_label_create(content);
    lv_obj_set_style_text_font(s_updates_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_updates_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_updates_status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_updates_status_label, "");
    lv_obj_add_flag(s_updates_status_label, LV_OBJ_FLAG_HIDDEN);

    // Download/flash progress - shown only while updates_install_confirm_cb()
    // is running. esp_https_ota's streaming API has no total-time estimate,
    // just bytes read vs. image size, so this is a plain 0-100% bar, not a
    // time-remaining countdown.
    s_updates_progress_row = lv_obj_create(content);
    lv_obj_remove_style_all(s_updates_progress_row);
    make_plain_container(s_updates_progress_row);
    lv_obj_set_size(s_updates_progress_row, button_width_px, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_updates_progress_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_updates_progress_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_updates_progress_row, 10, 0);
    lv_obj_add_flag(s_updates_progress_row, LV_OBJ_FLAG_HIDDEN);

    s_updates_progress_bar = lv_bar_create(s_updates_progress_row);
    lv_obj_set_flex_grow(s_updates_progress_bar, 1);
    lv_obj_set_height(s_updates_progress_bar, 12);
    lv_bar_set_range(s_updates_progress_bar, 0, 100);
    lv_bar_set_value(s_updates_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_updates_progress_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_updates_progress_bar, COLOR_HAIRLINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_updates_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_updates_progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_updates_progress_bar, 6, LV_PART_INDICATOR);
    // Muted vs. full COLOR_ACCENT - a saturated fill reads as too bright
    // filling a whole bar; same blue family as the "Update available" card
    // above, just toned down.
    lv_obj_set_style_bg_color(s_updates_progress_bar, lv_color_hex(0x3E86A0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_updates_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);

    s_updates_progress_label = lv_label_create(s_updates_progress_row);
    lv_obj_set_style_text_color(s_updates_progress_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_updates_progress_label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_updates_progress_label, 40);
    lv_label_set_text(s_updates_progress_label, "0%");

    // One button, always the same outline/muted style regardless of state -
    // only its label changes ("Check for updates" / "Checking..." /
    // "Install update", see updates_screen_reset()/updates_action_cb()).
    s_updates_action_button = lv_button_create(content);
    lv_obj_remove_style_all(s_updates_action_button);
    make_plain_container(s_updates_action_button);
    lv_obj_add_flag(s_updates_action_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_updates_action_button, button_width_px, 44);
    lv_obj_set_style_margin_top(s_updates_action_button, 4, 0);
    lv_obj_set_style_radius(s_updates_action_button, 9, 0);
    lv_obj_set_style_bg_color(s_updates_action_button, COLOR_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(s_updates_action_button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_updates_action_button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_flex_flow(s_updates_action_button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_updates_action_button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_updates_action_button, updates_action_cb, LV_EVENT_CLICKED, NULL);

    s_updates_action_label = lv_label_create(s_updates_action_button);
    lv_obj_set_style_text_color(s_updates_action_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_updates_action_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_updates_action_label, LV_SYMBOL_REFRESH " Check for updates");
}

static void about_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LIST);
}

// Static content built once - firmware version is read at build time and
// never changes without a reboot, so unlike the Updates screen there is no
// per-entry reset function.
static void build_about_screen(lv_obj_t *screen)
{
    s_about_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_about_screen);
    make_plain_container(s_about_screen);
    lv_obj_set_size(s_about_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_about_screen, LV_FLEX_FLOW_COLUMN);

    build_subscreen_header(s_about_screen, "About", NULL, about_back_cb);

    lv_obj_t *content = lv_obj_create(s_about_screen);
    lv_obj_remove_style_all(content);
    make_plain_container(content);
    lv_obj_set_size(content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(content, 20, 0);
    lv_obj_set_style_pad_right(content, 20, 0);
    lv_obj_set_style_pad_top(content, 16, 0);
    lv_obj_set_style_pad_row(content, 6, 0);

    lv_obj_t *name_label = lv_label_create(content);
    lv_obj_set_style_text_color(name_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_18, 0);
    lv_label_set_text(name_label, "ESP32 Market Data Terminal");

    const esp_app_desc_t *running = esp_app_get_description();
    lv_obj_t *version_label = lv_label_create(content);
    lv_obj_set_style_text_color(version_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_12, 0);
    lv_label_set_text_fmt(version_label, "Firmware v%s", running->version);

    lv_obj_t *license_label = lv_label_create(content);
    lv_obj_set_style_text_color(license_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(license_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_margin_top(license_label, 8, 0);
    lv_label_set_text(license_label, "Open Source - Apache License 2.0");

    lv_obj_t *attribution_label = lv_label_create(content);
    lv_obj_set_style_text_color(attribution_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(attribution_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(attribution_label, "Market data by Binance");

    lv_obj_t *divider = lv_obj_create(content);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(divider, COLOR_HAIRLINE, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(divider, 14, 0);
    lv_obj_set_style_margin_bottom(divider, 2, 0);

    // Same informational disclaimer body the first-run disclaimer modal
    // shows (Phase 14 slice 4) - reachable here at any time from Settings,
    // not just once on first boot after an update.
    lv_obj_t *disclaimer_label = lv_label_create(content);
    lv_obj_set_style_text_color(disclaimer_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(disclaimer_label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(disclaimer_label, LV_PCT(100));
    lv_label_set_long_mode(disclaimer_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(disclaimer_label,
                       "Market data is provided by Binance and may be delayed, incomplete, or "
                       "inaccurate. Nothing shown here is financial, investment, or trading "
                       "advice - always verify prices on the official exchange. The software "
                       "is provided \"as is\", without warranty of any kind.");
}

static void locale_row_click_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LOCALE);
}

static void watchlist_row_click_cb(lv_event_t *e)
{
    (void)e;
    watchlist_manage_rebuild();
    show_settings_view(SETTINGS_VIEW_WATCHLIST_MANAGE);
}

static void about_row_click_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_ABOUT);
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
    // the closest available placeholder, not a dedicated "time" icon.
    s_settings_locale_row_desc =
        build_settings_row(s_settings_list, LV_SYMBOL_SETTINGS, "Time", "--", locale_row_click_cb);
    s_settings_watchlist_row_desc =
        build_settings_row(s_settings_list, LV_SYMBOL_LIST, "Watchlist symbols", "--", watchlist_row_click_cb);
    s_settings_updates_row_desc =
        build_settings_row(s_settings_list, LV_SYMBOL_DOWNLOAD, "Updates", "--", updates_row_click_cb);
    // Static description, not a live-updated label like the rows above -
    // doubles as a passive Binance data attribution visible right on the
    // Settings list, without needing to enter the About screen.
    build_settings_row(s_settings_list, LV_SYMBOL_HOME, "About", "Market data by Binance", about_row_click_cb);
}

// --- Settings > Time > Date format ---

typedef struct
{
    const char *mask;   // human-readable pattern shown in the row, e.g. "DD/MM/YYYY"
    const char *format; // strftime() pattern that actually produces it
} date_format_option_t;

static const date_format_option_t s_date_formats[] = {
    { "DDD DD MMM", "%a %d %b" },
    { "DD MMM YYYY", "%d %b %Y" },
    { "MMM DD, YYYY", "%b %d, %Y" },
    { "DD/MM/YYYY", "%d/%m/%Y" },
    { "MM/DD/YYYY", "%m/%d/%Y" },
    { "YYYY/MM/DD", "%Y/%m/%d" },
    { "YYYY-MM-DD", "%Y-%m-%d" },
    { "DD-MM-YYYY", "%d-%m-%Y" },
    { "MM-DD-YYYY", "%m-%d-%Y" },
    { "DD.MM.YYYY", "%d.%m.%Y" },
    { "MM.DD.YYYY", "%m.%d.%Y" },
    { "YYYY.MM.DD", "%Y.%m.%d" },
};
#define DATE_FORMAT_COUNT (sizeof(s_date_formats) / sizeof(s_date_formats[0]))

// Parallel to s_date_formats[] - each row's checkmark, hidden/shown on
// selection (see date_format_row_click_cb()).
static lv_obj_t *s_date_format_checks[DATE_FORMAT_COUNT];

// --- Settings > Time > Time zones ---
//
// Region -> city -> POSIX TZ string, structured the same way as the
// sibling siswood-sc01-plus project's Settings > Time > Time zones (same
// region/city/POSIX data, just renamed to this file's s_/tz_ conventions).
// tz_entry_t.posix indexes s_tz_posix_strings[] rather than embedding the
// POSIX string directly, since most cities within a region share one DST
// rule (e.g. every CET/CEST city in Europe).

typedef enum {
    TZ_ZONE_AMERICA,
    TZ_ZONE_ASIA,
    TZ_ZONE_ATLANTIC,
    TZ_ZONE_AUSTRALIA,
    TZ_ZONE_AFRICA,
    TZ_ZONE_EUROPE,
    TZ_ZONE_INDIAN,
    TZ_ZONE_PACIFIC,
    TZ_ZONE_COUNT,
} tz_zone_id_t;

static const char *const s_tz_zone_names[TZ_ZONE_COUNT] = {
    [TZ_ZONE_AMERICA] = "America",
    [TZ_ZONE_ASIA] = "Asia",
    [TZ_ZONE_ATLANTIC] = "Atlantic",
    [TZ_ZONE_AUSTRALIA] = "Australia",
    [TZ_ZONE_AFRICA] = "Africa",
    [TZ_ZONE_EUROPE] = "Europe",
    [TZ_ZONE_INDIAN] = "Indian",
    [TZ_ZONE_PACIFIC] = "Pacific",
};

typedef enum {
    TZ_P_UTC0,
    TZ_P_GMT_BST,
    TZ_P_WET_WEST,
    TZ_P_AZOT,
    TZ_P_GMT_NO_DST,
    TZ_P_CET_CEST,
    TZ_P_EET_EEST,
    TZ_P_TRT,
    TZ_P_IST_IDT,
    TZ_P_MSK,
    TZ_P_GET,
    TZ_P_AMT_ARM,
    TZ_P_AZT,
    TZ_P_AST3,
    TZ_P_GST4,
    TZ_P_IRST,
    TZ_P_EET_NO_DST,
    TZ_P_SAST,
    TZ_P_CAT,
    TZ_P_EAT,
    TZ_P_CET_NO_DST,
    TZ_P_WAT,
    TZ_P_MUT,
    TZ_P_SCT,
    TZ_P_CVT,
    TZ_P_PKT,
    TZ_P_IST_INDIA,
    TZ_P_SLST,
    TZ_P_NPT,
    TZ_P_BDT,
    TZ_P_MMT,
    TZ_P_AQTT,
    TZ_P_ALMT,
    TZ_P_XJT,
    TZ_P_ICT,
    TZ_P_WIB,
    TZ_P_WITA,
    TZ_P_WIT,
    TZ_P_CST_CHINA,
    TZ_P_HKT,
    TZ_P_SGT,
    TZ_P_PHT,
    TZ_P_JST,
    TZ_P_KST,
    TZ_P_ULAT,
    TZ_P_VLAT,
    TZ_P_AWST,
    TZ_P_ACST,
    TZ_P_ACST_ACDT,
    TZ_P_AEST,
    TZ_P_AEST_AEDT,
    TZ_P_LHST,
    TZ_P_PGT,
    TZ_P_NCT,
    TZ_P_NZST_NZDT,
    TZ_P_FJT,
    TZ_P_TAHT,
    TZ_P_MART,
    TZ_P_AKST_AKDT,
    TZ_P_PST_PDT,
    TZ_P_MST_MDT,
    TZ_P_MST_NO_DST,
    TZ_P_CST_CDT,
    TZ_P_CST_CDT_MX,
    TZ_P_EST_EDT,
    TZ_P_AST_ADT,
    TZ_P_NST_NDT,
    TZ_P_HST,
    TZ_P_AST_NO_DST,
    TZ_P_CST5CDT_CUBA,
    TZ_P_CST6,
    TZ_P_EST5,
    TZ_P_COT,
    TZ_P_PET,
    TZ_P_ECT,
    TZ_P_VET,
    TZ_P_BOT,
    TZ_P_CLT_CLST,
    TZ_P_PYT_PYST,
    TZ_P_ART,
    TZ_P_UYT,
    TZ_P_BRT,
    TZ_P_AMT4,
    TZ_P_ACT5,
    TZ_P_CLST3,
    TZ_P_FKT3,
    TZ_P_WGT_WGST,
    TZ_P_CHST, // Guam / Northern Mariana Islands, no DST
    TZ_P_SST,  // American Samoa, no DST
} tz_posix_id_t;

static const char *const s_tz_posix_strings[] = {
    [TZ_P_UTC0] = "UTC0",
    [TZ_P_GMT_BST] = "GMT0BST,M3.5.0/1,M10.5.0/2",
    [TZ_P_WET_WEST] = "WET0WEST,M3.5.0/1,M10.5.0/2",
    [TZ_P_AZOT] = "AZOT1AZOST,M3.5.0/0,M10.5.0/1",
    [TZ_P_GMT_NO_DST] = "GMT0",
    [TZ_P_CET_CEST] = "CET-1CEST,M3.5.0/2,M10.5.0/3",
    [TZ_P_EET_EEST] = "EET-2EEST,M3.5.0/3,M10.5.0/4",
    [TZ_P_TRT] = "TRT-3",
    [TZ_P_IST_IDT] = "IST-2IDT,M3.4.4/26,M10.5.0",
    [TZ_P_MSK] = "MSK-3",
    [TZ_P_GET] = "GET-4",
    [TZ_P_AMT_ARM] = "AMT-4",
    [TZ_P_AZT] = "AZT-4",
    [TZ_P_AST3] = "AST-3",
    [TZ_P_GST4] = "GST-4",
    [TZ_P_IRST] = "IRST-3:30IRDT,M3.4.0/0,M9.4.0/0",
    [TZ_P_EET_NO_DST] = "EET-2",
    [TZ_P_SAST] = "SAST-2",
    [TZ_P_CAT] = "CAT-2",
    [TZ_P_EAT] = "EAT-3",
    [TZ_P_CET_NO_DST] = "CET-1",
    [TZ_P_WAT] = "WAT-1",
    [TZ_P_MUT] = "MUT-4",
    [TZ_P_SCT] = "SCT-4",
    [TZ_P_CVT] = "CVT1",
    [TZ_P_PKT] = "PKT-5",
    [TZ_P_IST_INDIA] = "IST-5:30",
    [TZ_P_SLST] = "SLST-5:30",
    [TZ_P_NPT] = "NPT-5:45",
    [TZ_P_BDT] = "BDT-6",
    [TZ_P_MMT] = "MMT-6:30",
    [TZ_P_AQTT] = "AQTT-5",
    [TZ_P_ALMT] = "ALMT-6",
    [TZ_P_XJT] = "XJT-6",
    [TZ_P_ICT] = "ICT-7",
    [TZ_P_WIB] = "WIB-7",
    [TZ_P_WITA] = "WITA-8",
    [TZ_P_WIT] = "WIT-9",
    [TZ_P_CST_CHINA] = "CST-8",
    [TZ_P_HKT] = "HKT-8",
    [TZ_P_SGT] = "SGT-8",
    [TZ_P_PHT] = "PHT-8",
    [TZ_P_JST] = "JST-9",
    [TZ_P_KST] = "KST-9",
    [TZ_P_ULAT] = "ULAT-8",
    [TZ_P_VLAT] = "VLAT-10",
    [TZ_P_AWST] = "AWST-8",
    [TZ_P_ACST] = "ACST-9:30",
    [TZ_P_ACST_ACDT] = "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3",
    [TZ_P_AEST] = "AEST-10",
    [TZ_P_AEST_AEDT] = "AEST-10AEDT,M10.1.0/2,M4.1.0/3",
    [TZ_P_LHST] = "LHST-10:30LHDT-11,M10.1.0/2,M4.1.0/2",
    [TZ_P_PGT] = "PGT-10",
    [TZ_P_NCT] = "NCT-11",
    [TZ_P_NZST_NZDT] = "NZST-12NZDT,M9.4.0/2,M4.1.0/3",
    [TZ_P_FJT] = "FJT-12",
    [TZ_P_TAHT] = "TAHT10",
    [TZ_P_MART] = "MART9:30",
    [TZ_P_AKST_AKDT] = "AKST9AKDT,M3.2.0/2,M11.1.0/2",
    [TZ_P_PST_PDT] = "PST8PDT,M3.2.0/2,M11.1.0/2",
    [TZ_P_MST_MDT] = "MST7MDT,M3.2.0/2,M11.1.0/2",
    [TZ_P_MST_NO_DST] = "MST7",
    [TZ_P_CST_CDT] = "CST6CDT,M3.2.0/2,M11.1.0/2",
    [TZ_P_CST_CDT_MX] = "CST6CDT,M4.1.0/2,M10.5.0/2",
    [TZ_P_EST_EDT] = "EST5EDT,M3.2.0/2,M11.1.0/2",
    [TZ_P_AST_ADT] = "AST4ADT,M3.2.0/2,M11.1.0/2",
    [TZ_P_NST_NDT] = "NST3:30NDT,M3.2.0/0,M11.1.0/0",
    [TZ_P_HST] = "HST10",
    [TZ_P_AST_NO_DST] = "AST4",
    [TZ_P_CST5CDT_CUBA] = "CST5CDT,M3.2.0/0,M11.1.0/0",
    [TZ_P_CST6] = "CST6",
    [TZ_P_EST5] = "EST5",
    [TZ_P_COT] = "COT5",
    [TZ_P_PET] = "PET5",
    [TZ_P_ECT] = "ECT5",
    [TZ_P_VET] = "VET4:30",
    [TZ_P_BOT] = "BOT4",
    [TZ_P_CLT_CLST] = "CLT4CLST,M9.1.0/0,M4.1.0/0",
    [TZ_P_PYT_PYST] = "PYT4PYST,M10.1.0/0,M3.4.0/0",
    [TZ_P_ART] = "ART3",
    [TZ_P_UYT] = "UYT3",
    [TZ_P_BRT] = "BRT3",
    [TZ_P_AMT4] = "AMT4",
    [TZ_P_ACT5] = "ACT5",
    [TZ_P_CLST3] = "CLST3",
    [TZ_P_FKT3] = "FKT3",
    [TZ_P_WGT_WGST] = "WGT3WGST,M3.5.0/22,M10.5.0/23",
    [TZ_P_CHST] = "ChST-10",
    [TZ_P_SST] = "SST11",
};

typedef enum {
    TZ_C_ACC_RA,
    TZ_C_ADDIS_ABABA,
    TZ_C_ADELAIDE,
    TZ_C_AQTAU,
    TZ_C_ALGIERS,
    TZ_C_ALMATY,
    TZ_C_AMSTERDAM,
    TZ_C_ANCHORAGE,
    TZ_C_ASUNCION,
    TZ_C_ASTANA,
    TZ_C_ATHENS,
    TZ_C_AUCKLAND,
    TZ_C_AZORES,
    TZ_C_BAGHDAD,
    TZ_C_BAKU,
    TZ_C_BALI,
    TZ_C_BANGKOK,
    TZ_C_BEIJING,
    TZ_C_BELGRADE,
    TZ_C_BERLIN,
    TZ_C_BOGOTA,
    TZ_C_BRATISLAVA,
    TZ_C_BRISBANE,
    TZ_C_BRUSSELS,
    TZ_C_BUCHAREST,
    TZ_C_BUENOS_AIRES,
    TZ_C_BUDAPEST,
    TZ_C_CAIRO,
    TZ_C_CALGARY,
    TZ_C_CANARY,
    TZ_C_CAPE_VERDE,
    TZ_C_CARACAS,
    TZ_C_CASABLANCA,
    TZ_C_CHICAGO,
    TZ_C_COLOMBO,
    TZ_C_COPENHAGEN,
    TZ_C_DAKAR,
    TZ_C_DALLAS,
    TZ_C_DARWIN,
    TZ_C_DENVER,
    TZ_C_DOHA,
    TZ_C_DUBAI,
    TZ_C_DHAKA,
    TZ_C_DUBLIN,
    TZ_C_FIJI,
    TZ_C_FORTALEZA,
    TZ_C_GUAYAQUIL,
    TZ_C_HALIFAX,
    TZ_C_HAVANA,
    TZ_C_HELSINKI,
    TZ_C_HOBART,
    TZ_C_HONG_KONG,
    TZ_C_HONOLULU,
    TZ_C_HO_CHI_MINH,
    TZ_C_ICELAND,
    TZ_C_ISTANBUL,
    TZ_C_JAKARTA,
    TZ_C_JAMAICA,
    TZ_C_JAYAPURA,
    TZ_C_JOHANNESBURG,
    TZ_C_KARACHI,
    TZ_C_KATHMANDU,
    TZ_C_KIEV,
    TZ_C_KUALA_LUMPUR,
    TZ_C_KOLKATA,
    TZ_C_KUWAIT,
    TZ_C_LAGOS,
    TZ_C_LA_PAZ,
    TZ_C_LIMA,
    TZ_C_LISBON,
    TZ_C_LJUBLJANA,
    TZ_C_LONDON,
    TZ_C_LORD_HOWE,
    TZ_C_LOS_ANGELES,
    TZ_C_MADRID,
    TZ_C_MAKASSAR,
    TZ_C_MANAUS,
    TZ_C_MANILA,
    TZ_C_MARQUESAS,
    TZ_C_MELBOURNE,
    TZ_C_MEXICO_CITY,
    TZ_C_MILAN,
    TZ_C_MINSK,
    TZ_C_MONTEVIDEO,
    TZ_C_MOSCOW,
    TZ_C_MUMBAI,
    TZ_C_NAIROBI,
    TZ_C_NEW_YORK,
    TZ_C_NICOSIA,
    TZ_C_NOUMEA,
    TZ_C_NUUK,
    TZ_C_OSLO,
    TZ_C_PARIS,
    TZ_C_PANAMA,
    TZ_C_PERTH,
    TZ_C_PHNOM_PENH,
    TZ_C_PHOENIX,
    TZ_C_PORT_LOUIS,
    TZ_C_PORT_MORESBY,
    TZ_C_PRAGUE,
    TZ_C_PUNTA_ARENAS,
    TZ_C_QUITO,
    TZ_C_RIYADH,
    TZ_C_RIO_DE_JANEIRO,
    TZ_C_RECIFE,
    TZ_C_RIGA,
    TZ_C_ROME,
    TZ_C_RIO_BRANCO,
    TZ_C_SAN_JUAN,
    TZ_C_SANTIAGO,
    TZ_C_SAO_PAULO,
    TZ_C_SARAJEVO,
    TZ_C_SEOUL,
    TZ_C_SHANGHAI,
    TZ_C_SINGAPORE,
    TZ_C_SKOPJE,
    TZ_C_SOFIA,
    TZ_C_ST_JOHNS,
    TZ_C_STANLEY,
    TZ_C_STOCKHOLM,
    TZ_C_SYDNEY,
    TZ_C_TAIPEI,
    TZ_C_TAHITI,
    TZ_C_TBILISI,
    TZ_C_TEL_AVIV,
    TZ_C_TEHRAN,
    TZ_C_TOKYO,
    TZ_C_TORONTO,
    TZ_C_TUNIS,
    TZ_C_ULAANBAATAR,
    TZ_C_URUMQI,
    TZ_C_VANCOUVER,
    TZ_C_VICTORIA,
    TZ_C_VIENNA,
    TZ_C_VIENTIANE,
    TZ_C_VILNIUS,
    TZ_C_VLADIVOSTOK,
    TZ_C_WELLINGTON,
    TZ_C_WINDHOEK,
    TZ_C_WARSAW,
    TZ_C_WINNIPEG,
    TZ_C_YANGON,
    TZ_C_YEREVAN,
    TZ_C_ZAGREB,
    TZ_C_ZURICH,
    TZ_C_GUATEMALA,
    TZ_C_GUAM,
    TZ_C_ST_THOMAS,
    TZ_C_PAGO_PAGO,
    TZ_C_SAIPAN
} tz_city_id_t;

static const char *const s_tz_city_names[] = {
    [TZ_C_ACC_RA] = "Accra",
    [TZ_C_ADDIS_ABABA] = "Addis Ababa",
    [TZ_C_ADELAIDE] = "Adelaide",
    [TZ_C_AQTAU] = "Aqtau",
    [TZ_C_ALGIERS] = "Algiers",
    [TZ_C_ALMATY] = "Almaty",
    [TZ_C_AMSTERDAM] = "Amsterdam",
    [TZ_C_ANCHORAGE] = "Anchorage",
    [TZ_C_ASUNCION] = "Asuncion",
    [TZ_C_ASTANA] = "Astana",
    [TZ_C_ATHENS] = "Athens",
    [TZ_C_AUCKLAND] = "Auckland",
    [TZ_C_AZORES] = "Azores",
    [TZ_C_BAGHDAD] = "Baghdad",
    [TZ_C_BAKU] = "Baku",
    [TZ_C_BALI] = "Bali",
    [TZ_C_BANGKOK] = "Bangkok",
    [TZ_C_BEIJING] = "Beijing",
    [TZ_C_BELGRADE] = "Belgrade",
    [TZ_C_BERLIN] = "Berlin",
    [TZ_C_BOGOTA] = "Bogota",
    [TZ_C_BRATISLAVA] = "Bratislava",
    [TZ_C_BRISBANE] = "Brisbane",
    [TZ_C_BRUSSELS] = "Brussels",
    [TZ_C_BUCHAREST] = "Bucharest",
    [TZ_C_BUENOS_AIRES] = "Buenos Aires",
    [TZ_C_BUDAPEST] = "Budapest",
    [TZ_C_CAIRO] = "Cairo",
    [TZ_C_CALGARY] = "Calgary",
    [TZ_C_CANARY] = "Canary",
    [TZ_C_CAPE_VERDE] = "Cape Verde",
    [TZ_C_CARACAS] = "Caracas",
    [TZ_C_CASABLANCA] = "Casablanca",
    [TZ_C_CHICAGO] = "Chicago",
    [TZ_C_COLOMBO] = "Colombo",
    [TZ_C_COPENHAGEN] = "Copenhagen",
    [TZ_C_DAKAR] = "Dakar",
    [TZ_C_DALLAS] = "Dallas",
    [TZ_C_DARWIN] = "Darwin",
    [TZ_C_DENVER] = "Denver",
    [TZ_C_DOHA] = "Doha",
    [TZ_C_DUBAI] = "Dubai",
    [TZ_C_DHAKA] = "Dhaka",
    [TZ_C_DUBLIN] = "Dublin",
    [TZ_C_FIJI] = "Fiji",
    [TZ_C_FORTALEZA] = "Fortaleza",
    [TZ_C_GUAYAQUIL] = "Guayaquil",
    [TZ_C_HALIFAX] = "Halifax",
    [TZ_C_HAVANA] = "Havana",
    [TZ_C_HELSINKI] = "Helsinki",
    [TZ_C_HOBART] = "Hobart",
    [TZ_C_HONG_KONG] = "Hong Kong",
    [TZ_C_HONOLULU] = "Honolulu",
    [TZ_C_HO_CHI_MINH] = "Ho Chi Minh",
    [TZ_C_ICELAND] = "Iceland",
    [TZ_C_ISTANBUL] = "Istanbul",
    [TZ_C_JAKARTA] = "Jakarta",
    [TZ_C_JAMAICA] = "Jamaica",
    [TZ_C_JAYAPURA] = "Jayapura",
    [TZ_C_JOHANNESBURG] = "Johannesburg",
    [TZ_C_KARACHI] = "Karachi",
    [TZ_C_KATHMANDU] = "Kathmandu",
    [TZ_C_KIEV] = "Kyiv",
    [TZ_C_KUALA_LUMPUR] = "Kuala Lumpur",
    [TZ_C_KOLKATA] = "Kolkata",
    [TZ_C_KUWAIT] = "Kuwait",
    [TZ_C_LAGOS] = "Lagos",
    [TZ_C_LA_PAZ] = "La Paz",
    [TZ_C_LIMA] = "Lima",
    [TZ_C_LISBON] = "Lisbon",
    [TZ_C_LJUBLJANA] = "Ljubljana",
    [TZ_C_LONDON] = "London",
    [TZ_C_LORD_HOWE] = "Lord Howe",
    [TZ_C_LOS_ANGELES] = "Los Angeles",
    [TZ_C_MADRID] = "Madrid",
    [TZ_C_MAKASSAR] = "Makassar",
    [TZ_C_MANAUS] = "Manaus",
    [TZ_C_MANILA] = "Manila",
    [TZ_C_MARQUESAS] = "Marquesas",
    [TZ_C_MELBOURNE] = "Melbourne",
    [TZ_C_MEXICO_CITY] = "Mexico City",
    [TZ_C_MILAN] = "Milan",
    [TZ_C_MINSK] = "Minsk",
    [TZ_C_MONTEVIDEO] = "Montevideo",
    [TZ_C_MOSCOW] = "Moscow",
    [TZ_C_MUMBAI] = "Mumbai",
    [TZ_C_NAIROBI] = "Nairobi",
    [TZ_C_NEW_YORK] = "New York",
    [TZ_C_NICOSIA] = "Nicosia",
    [TZ_C_NOUMEA] = "Noumea",
    [TZ_C_NUUK] = "Nuuk",
    [TZ_C_OSLO] = "Oslo",
    [TZ_C_PARIS] = "Paris",
    [TZ_C_PANAMA] = "Panama",
    [TZ_C_PERTH] = "Perth",
    [TZ_C_PHNOM_PENH] = "Phnom Penh",
    [TZ_C_PHOENIX] = "Phoenix",
    [TZ_C_PORT_LOUIS] = "Port Louis",
    [TZ_C_PORT_MORESBY] = "Port Moresby",
    [TZ_C_PRAGUE] = "Prague",
    [TZ_C_PUNTA_ARENAS] = "Punta Arenas",
    [TZ_C_QUITO] = "Quito",
    [TZ_C_RIYADH] = "Riyadh",
    [TZ_C_RIO_DE_JANEIRO] = "Rio de Janeiro",
    [TZ_C_RECIFE] = "Recife",
    [TZ_C_ROME] = "Rome",
    [TZ_C_RIGA] = "Riga",
    [TZ_C_RIO_BRANCO] = "Rio Branco",
    [TZ_C_SAN_JUAN] = "San Juan",
    [TZ_C_SANTIAGO] = "Santiago",
    [TZ_C_SAO_PAULO] = "Sao Paulo",
    [TZ_C_SARAJEVO] = "Sarajevo",
    [TZ_C_SEOUL] = "Seoul",
    [TZ_C_SHANGHAI] = "Shanghai",
    [TZ_C_SINGAPORE] = "Singapore",
    [TZ_C_SKOPJE] = "Skopje",
    [TZ_C_SOFIA] = "Sofia",
    [TZ_C_ST_JOHNS] = "St Johns",
    [TZ_C_STANLEY] = "Stanley",
    [TZ_C_STOCKHOLM] = "Stockholm",
    [TZ_C_SYDNEY] = "Sydney",
    [TZ_C_TAIPEI] = "Taipei",
    [TZ_C_TAHITI] = "Tahiti",
    [TZ_C_TBILISI] = "Tbilisi",
    [TZ_C_TEL_AVIV] = "Tel Aviv",
    [TZ_C_TEHRAN] = "Tehran",
    [TZ_C_TOKYO] = "Tokyo",
    [TZ_C_TORONTO] = "Toronto",
    [TZ_C_TUNIS] = "Tunis",
    [TZ_C_ULAANBAATAR] = "Ulaanbaatar",
    [TZ_C_URUMQI] = "Urumqi",
    [TZ_C_VANCOUVER] = "Vancouver",
    [TZ_C_VICTORIA] = "Victoria",
    [TZ_C_VIENNA] = "Vienna",
    [TZ_C_VIENTIANE] = "Vientiane",
    [TZ_C_VILNIUS] = "Vilnius",
    [TZ_C_VLADIVOSTOK] = "Vladivostok",
    [TZ_C_WELLINGTON] = "Wellington",
    [TZ_C_WINDHOEK] = "Windhoek",
    [TZ_C_WARSAW] = "Warsaw",
    [TZ_C_WINNIPEG] = "Winnipeg",
    [TZ_C_YANGON] = "Yangon",
    [TZ_C_YEREVAN] = "Yerevan",
    [TZ_C_ZAGREB] = "Zagreb",
    [TZ_C_ZURICH] = "Zurich",
    [TZ_C_GUATEMALA] = "Guatemala",
    [TZ_C_GUAM] = "Guam",
    [TZ_C_ST_THOMAS] = "St Thomas",
    [TZ_C_PAGO_PAGO] = "Pago Pago",
    [TZ_C_SAIPAN] = "Saipan",
};

typedef struct
{
    tz_zone_id_t zone;
    uint8_t city;
    tz_posix_id_t posix;
} tz_entry_t;

#define TZROW(zone, city, posix) { zone, city, posix }

// Not const: sorted in place, once, the first time build_time_zone_screen()
// runs (see its qsort() call) so region/city lists read alphabetically
// instead of in this table's ad-hoc insertion order.
static tz_entry_t s_timezones[] = {
    // UTC / Atlantic
    TZROW(TZ_ZONE_EUROPE, TZ_C_LONDON, TZ_P_GMT_BST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_DUBLIN, TZ_P_GMT_BST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_LISBON, TZ_P_WET_WEST),
    TZROW(TZ_ZONE_ATLANTIC, TZ_C_CANARY, TZ_P_WET_WEST),
    TZROW(TZ_ZONE_ATLANTIC, TZ_C_AZORES, TZ_P_AZOT),
    TZROW(TZ_ZONE_ATLANTIC, TZ_C_ICELAND, TZ_P_GMT_NO_DST),
    TZROW(TZ_ZONE_ATLANTIC, TZ_C_CAPE_VERDE, TZ_P_CVT),

    // Europe (CET/EET/others)
    TZROW(TZ_ZONE_EUROPE, TZ_C_MADRID, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_PARIS, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_BERLIN, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_VIENNA, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_ROME, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_MILAN, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_AMSTERDAM, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_BRUSSELS, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_ZURICH, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_STOCKHOLM, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_OSLO, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_COPENHAGEN, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_WARSAW, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_PRAGUE, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_BRATISLAVA, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_BUDAPEST, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_ZAGREB, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_BELGRADE, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_LJUBLJANA, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_SARAJEVO, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_SKOPJE, TZ_P_CET_CEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_KIEV, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_ATHENS, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_BUCHAREST, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_SOFIA, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_HELSINKI, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_RIGA, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_VILNIUS, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_ASIA, TZ_C_NICOSIA, TZ_P_EET_EEST),
    TZROW(TZ_ZONE_EUROPE, TZ_C_ISTANBUL, TZ_P_TRT),
    TZROW(TZ_ZONE_ASIA, TZ_C_TEL_AVIV, TZ_P_IST_IDT),
    TZROW(TZ_ZONE_EUROPE, TZ_C_MOSCOW, TZ_P_MSK),
    TZROW(TZ_ZONE_EUROPE, TZ_C_MINSK, TZ_P_MSK),
    TZROW(TZ_ZONE_ASIA, TZ_C_TBILISI, TZ_P_GET),
    TZROW(TZ_ZONE_ASIA, TZ_C_YEREVAN, TZ_P_AMT_ARM),
    TZROW(TZ_ZONE_ASIA, TZ_C_BAKU, TZ_P_AZT),

    // Middle East / Africa
    TZROW(TZ_ZONE_ASIA, TZ_C_RIYADH, TZ_P_AST3),
    TZROW(TZ_ZONE_ASIA, TZ_C_DOHA, TZ_P_AST3),
    TZROW(TZ_ZONE_ASIA, TZ_C_DUBAI, TZ_P_GST4),
    TZROW(TZ_ZONE_ASIA, TZ_C_KUWAIT, TZ_P_AST3),
    TZROW(TZ_ZONE_ASIA, TZ_C_BAGHDAD, TZ_P_AST3),
    TZROW(TZ_ZONE_ASIA, TZ_C_TEHRAN, TZ_P_IRST),
    TZROW(TZ_ZONE_AFRICA, TZ_C_CAIRO, TZ_P_EET_NO_DST),
    TZROW(TZ_ZONE_AFRICA, TZ_C_JOHANNESBURG, TZ_P_SAST),
    TZROW(TZ_ZONE_AFRICA, TZ_C_WINDHOEK, TZ_P_CAT),
    TZROW(TZ_ZONE_AFRICA, TZ_C_NAIROBI, TZ_P_EAT),
    TZROW(TZ_ZONE_AFRICA, TZ_C_ADDIS_ABABA, TZ_P_EAT),
    TZROW(TZ_ZONE_AFRICA, TZ_C_CASABLANCA, TZ_P_WET_WEST),
    TZROW(TZ_ZONE_AFRICA, TZ_C_ALGIERS, TZ_P_CET_NO_DST),
    TZROW(TZ_ZONE_AFRICA, TZ_C_TUNIS, TZ_P_CET_NO_DST),
    TZROW(TZ_ZONE_AFRICA, TZ_C_LAGOS, TZ_P_WAT),
    TZROW(TZ_ZONE_AFRICA, TZ_C_DAKAR, TZ_P_GMT_NO_DST),
    TZROW(TZ_ZONE_INDIAN, TZ_C_PORT_LOUIS, TZ_P_MUT),
    TZROW(TZ_ZONE_INDIAN, TZ_C_VICTORIA, TZ_P_SCT),

    // South Asia / Central Asia
    TZROW(TZ_ZONE_ASIA, TZ_C_KARACHI, TZ_P_PKT),
    TZROW(TZ_ZONE_ASIA, TZ_C_KOLKATA, TZ_P_IST_INDIA),
    TZROW(TZ_ZONE_ASIA, TZ_C_MUMBAI, TZ_P_IST_INDIA),
    TZROW(TZ_ZONE_ASIA, TZ_C_COLOMBO, TZ_P_SLST),
    TZROW(TZ_ZONE_ASIA, TZ_C_KATHMANDU, TZ_P_NPT),
    TZROW(TZ_ZONE_ASIA, TZ_C_DHAKA, TZ_P_BDT),
    TZROW(TZ_ZONE_ASIA, TZ_C_YANGON, TZ_P_MMT),
    TZROW(TZ_ZONE_ASIA, TZ_C_AQTAU, TZ_P_AQTT),
    TZROW(TZ_ZONE_ASIA, TZ_C_ASTANA, TZ_P_ALMT),
    TZROW(TZ_ZONE_ASIA, TZ_C_ALMATY, TZ_P_ALMT),
    TZROW(TZ_ZONE_ASIA, TZ_C_URUMQI, TZ_P_XJT),

    // Southeast / East Asia
    TZROW(TZ_ZONE_ASIA, TZ_C_BANGKOK, TZ_P_ICT),
    TZROW(TZ_ZONE_ASIA, TZ_C_HO_CHI_MINH, TZ_P_ICT),
    TZROW(TZ_ZONE_ASIA, TZ_C_PHNOM_PENH, TZ_P_ICT),
    TZROW(TZ_ZONE_ASIA, TZ_C_VIENTIANE, TZ_P_ICT),
    TZROW(TZ_ZONE_ASIA, TZ_C_JAKARTA, TZ_P_WIB),
    TZROW(TZ_ZONE_ASIA, TZ_C_BALI, TZ_P_WITA),
    TZROW(TZ_ZONE_ASIA, TZ_C_MAKASSAR, TZ_P_WITA),
    TZROW(TZ_ZONE_ASIA, TZ_C_JAYAPURA, TZ_P_WIT),
    TZROW(TZ_ZONE_ASIA, TZ_C_BEIJING, TZ_P_CST_CHINA),
    TZROW(TZ_ZONE_ASIA, TZ_C_SHANGHAI, TZ_P_CST_CHINA),
    TZROW(TZ_ZONE_ASIA, TZ_C_HONG_KONG, TZ_P_HKT),
    TZROW(TZ_ZONE_ASIA, TZ_C_TAIPEI, TZ_P_CST_CHINA),
    TZROW(TZ_ZONE_ASIA, TZ_C_SINGAPORE, TZ_P_SGT),
    TZROW(TZ_ZONE_ASIA, TZ_C_KUALA_LUMPUR, TZ_P_SGT),
    TZROW(TZ_ZONE_ASIA, TZ_C_MANILA, TZ_P_PHT),
    TZROW(TZ_ZONE_ASIA, TZ_C_TOKYO, TZ_P_JST),
    TZROW(TZ_ZONE_ASIA, TZ_C_SEOUL, TZ_P_KST),
    TZROW(TZ_ZONE_ASIA, TZ_C_ULAANBAATAR, TZ_P_ULAT),
    TZROW(TZ_ZONE_ASIA, TZ_C_VLADIVOSTOK, TZ_P_VLAT),

    // Oceania
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_PERTH, TZ_P_AWST),
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_DARWIN, TZ_P_ACST),
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_ADELAIDE, TZ_P_ACST_ACDT),
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_BRISBANE, TZ_P_AEST),
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_SYDNEY, TZ_P_AEST_AEDT),
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_MELBOURNE, TZ_P_AEST_AEDT),
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_HOBART, TZ_P_AEST_AEDT),
    TZROW(TZ_ZONE_AUSTRALIA, TZ_C_LORD_HOWE, TZ_P_LHST),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_PORT_MORESBY, TZ_P_PGT),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_NOUMEA, TZ_P_NCT),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_AUCKLAND, TZ_P_NZST_NZDT),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_WELLINGTON, TZ_P_NZST_NZDT),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_FIJI, TZ_P_FJT),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_TAHITI, TZ_P_TAHT),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_MARQUESAS, TZ_P_MART),

    // North America
    TZROW(TZ_ZONE_AMERICA, TZ_C_ANCHORAGE, TZ_P_AKST_AKDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_LOS_ANGELES, TZ_P_PST_PDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_VANCOUVER, TZ_P_PST_PDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_DENVER, TZ_P_MST_MDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_CALGARY, TZ_P_MST_MDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_PHOENIX, TZ_P_MST_NO_DST),
    TZROW(TZ_ZONE_AMERICA, TZ_C_CHICAGO, TZ_P_CST_CDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_DALLAS, TZ_P_CST_CDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_WINNIPEG, TZ_P_CST_CDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_MEXICO_CITY, TZ_P_CST_CDT_MX),
    TZROW(TZ_ZONE_AMERICA, TZ_C_NEW_YORK, TZ_P_EST_EDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_TORONTO, TZ_P_EST_EDT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_HALIFAX, TZ_P_AST_ADT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_ST_JOHNS, TZ_P_NST_NDT),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_HONOLULU, TZ_P_HST),
    TZROW(TZ_ZONE_AMERICA, TZ_C_SAN_JUAN, TZ_P_AST_NO_DST),
    TZROW(TZ_ZONE_AMERICA, TZ_C_ST_THOMAS, TZ_P_AST_NO_DST),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_GUAM, TZ_P_CHST),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_SAIPAN, TZ_P_CHST),
    TZROW(TZ_ZONE_PACIFIC, TZ_C_PAGO_PAGO, TZ_P_SST),
    TZROW(TZ_ZONE_AMERICA, TZ_C_HAVANA, TZ_P_CST5CDT_CUBA),

    // Central / South America
    TZROW(TZ_ZONE_AMERICA, TZ_C_GUATEMALA, TZ_P_CST6),
    TZROW(TZ_ZONE_AMERICA, TZ_C_PANAMA, TZ_P_EST5),
    TZROW(TZ_ZONE_AMERICA, TZ_C_JAMAICA, TZ_P_EST5),
    TZROW(TZ_ZONE_AMERICA, TZ_C_BOGOTA, TZ_P_COT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_LIMA, TZ_P_PET),
    TZROW(TZ_ZONE_AMERICA, TZ_C_QUITO, TZ_P_ECT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_CARACAS, TZ_P_VET),
    TZROW(TZ_ZONE_AMERICA, TZ_C_LA_PAZ, TZ_P_BOT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_SANTIAGO, TZ_P_CLT_CLST),
    TZROW(TZ_ZONE_AMERICA, TZ_C_ASUNCION, TZ_P_PYT_PYST),
    TZROW(TZ_ZONE_AMERICA, TZ_C_BUENOS_AIRES, TZ_P_ART),
    TZROW(TZ_ZONE_AMERICA, TZ_C_MONTEVIDEO, TZ_P_UYT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_SAO_PAULO, TZ_P_BRT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_RIO_DE_JANEIRO, TZ_P_BRT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_FORTALEZA, TZ_P_BRT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_RECIFE, TZ_P_BRT),
    TZROW(TZ_ZONE_AMERICA, TZ_C_MANAUS, TZ_P_AMT4),
    TZROW(TZ_ZONE_AMERICA, TZ_C_RIO_BRANCO, TZ_P_ACT5),
    TZROW(TZ_ZONE_AMERICA, TZ_C_PUNTA_ARENAS, TZ_P_CLST3),
    TZROW(TZ_ZONE_ATLANTIC, TZ_C_STANLEY, TZ_P_FKT3),
    TZROW(TZ_ZONE_AMERICA, TZ_C_NUUK, TZ_P_WGT_WGST),
};
#define TZ_TABLE_COUNT (sizeof(s_timezones) / sizeof(s_timezones[0]))

static int tz_compare(const void *a, const void *b)
{
    const tz_entry_t *ea = (const tz_entry_t *)a;
    const tz_entry_t *eb = (const tz_entry_t *)b;
    int c = strcmp(s_tz_zone_names[ea->zone], s_tz_zone_names[eb->zone]);
    if (c != 0)
    {
        return c;
    }
    return strcmp(s_tz_city_names[ea->city], s_tz_city_names[eb->city]);
}

static size_t tz_count_in_zone(tz_zone_id_t zone)
{
    size_t count = 0;
    for (size_t i = 0; i < TZ_TABLE_COUNT; i++)
    {
        if (s_timezones[i].zone == zone)
        {
            count++;
        }
    }
    return count;
}

// Settings root's "Time" row description: the exact "Zone/City" the user
// picked (see time_zone_city_row_click_cb()), or a short fallback when
// nothing has been picked yet (e.g. the "UTC0" default, which has no city
// entry of its own).
static void tz_describe(const char *tz_label, const char *posix, char *out, size_t out_len)
{
    if (tz_label[0] != '\0')
    {
        snprintf(out, out_len, "%s", tz_label);
        return;
    }
    if (strcmp(posix, "UTC0") == 0)
    {
        snprintf(out, out_len, "UTC");
        return;
    }
    snprintf(out, out_len, "%s", posix);
}

// --- Settings > Time screens ---

static void time_format_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LOCALE);
}

static void date_format_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LOCALE);
}

static void time_zone_region_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LOCALE);
}

static void time_zone_city_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_TIME_ZONES);
}

// A selectable row whose checkmark is a fixed child created up front and
// then just hidden/shown, rather than created/destroyed on selection - used
// by the two fixed-length lists (Time format, Date format). out_check lets
// the caller keep a handle to toggle it later.
static lv_obj_t *build_time_toggle_row(lv_obj_t *parent, const char *title, lv_event_cb_t click_cb, void *user_data,
                                        lv_obj_t **out_check)
{
    lv_obj_t *row = build_selectable_row(parent, title, false, click_cb, user_data);
    lv_obj_t *check = lv_label_create(row);
    lv_obj_set_style_text_color(check, COLOR_ACCENT, 0);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
    if (out_check)
    {
        *out_check = check;
    }
    return row;
}

static void time_format_refresh_marks(void)
{
    if (s_locale.time_24h)
    {
        lv_obj_remove_flag(s_time_format_check_24h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_format_check_12h, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_remove_flag(s_time_format_check_12h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_format_check_24h, LV_OBJ_FLAG_HIDDEN);
    }
}

static void time_format_row_click_cb(lv_event_t *e)
{
    bool is_24h = (bool)(uintptr_t)lv_event_get_user_data(e);
    s_locale.time_24h = is_24h;
    settings_store_save_locale(&s_locale);
    time_format_refresh_marks();
}

static void build_time_format_screen(lv_obj_t *screen)
{
    lv_obj_t *body =
        build_time_subscreen(screen, &s_time_format_screen, "Time format", NULL, NULL, time_format_back_cb);

    build_time_toggle_row(body, "12-hour", time_format_row_click_cb, (void *)(uintptr_t)false,
                           &s_time_format_check_12h);
    build_time_toggle_row(body, "24-hour", time_format_row_click_cb, (void *)(uintptr_t)true,
                           &s_time_format_check_24h);

    time_format_refresh_marks();
}

static void date_format_row_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    strncpy(s_locale.date_format, s_date_formats[idx].format, SETTINGS_DATE_FORMAT_MAX_LEN);
    s_locale.date_format[SETTINGS_DATE_FORMAT_MAX_LEN] = '\0';
    settings_store_save_locale(&s_locale);

    for (size_t i = 0; i < DATE_FORMAT_COUNT; i++)
    {
        if (i == idx)
        {
            lv_obj_remove_flag(s_date_format_checks[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_date_format_checks[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void build_date_format_screen(lv_obj_t *screen)
{
    lv_obj_t *body =
        build_time_subscreen(screen, &s_date_format_screen, "Date format", NULL, NULL, date_format_back_cb);

    for (size_t i = 0; i < DATE_FORMAT_COUNT; i++)
    {
        build_time_toggle_row(body, s_date_formats[i].mask, date_format_row_click_cb, (void *)(uintptr_t)i,
                               &s_date_format_checks[i]);
    }

    for (size_t i = 0; i < DATE_FORMAT_COUNT; i++)
    {
        if (strcmp(s_locale.date_format, s_date_formats[i].format) == 0)
        {
            lv_obj_remove_flag(s_date_format_checks[i], LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }
}

static void time_zone_city_row_click_cb(lv_event_t *e); // defined below, used by show_time_zone_cities()
static void apply_region_change(settings_api_region_t region,
                                 settings_api_region_source_t source); // defined below, used by time_zone_city_row_click_cb

// Rebuilds the city list for one region from scratch - cheap enough to redo
// on every tap (at most a few dozen rows) and far simpler than moving a
// single checkmark between rows that get torn down and recreated anyway.
static void show_time_zone_cities(tz_zone_id_t zone)
{
    lv_label_set_text(s_time_zone_city_subtitle, s_tz_zone_names[zone]);
    lv_obj_clean(s_time_zone_city_list);

    for (size_t i = 0; i < TZ_TABLE_COUNT; i++)
    {
        const tz_entry_t *entry = &s_timezones[i];
        if (entry->zone != zone)
        {
            continue;
        }
        char title_buf[48];
        snprintf(title_buf, sizeof(title_buf), "%s/%s", s_tz_zone_names[entry->zone], s_tz_city_names[entry->city]);
        // Matched on the exact "Zone/City" label the user picked, not on
        // posix_tz - several cities in a region can share one POSIX/DST
        // rule (e.g. every CET/CEST city in Europe), which previously made
        // every one of them light up as "current" at once.
        bool is_current = strcmp(title_buf, s_locale.tz_label) == 0;
        build_selectable_row(s_time_zone_city_list, title_buf, is_current, time_zone_city_row_click_cb,
                              (void *)entry);
    }
}

static void time_zone_city_row_click_cb(lv_event_t *e)
{
    const tz_entry_t *entry = (const tz_entry_t *)lv_event_get_user_data(e);
    if (!entry)
    {
        return;
    }
    const char *posix = s_tz_posix_strings[entry->posix];

    strncpy(s_locale.posix_tz, posix, SETTINGS_POSIX_TZ_MAX_LEN);
    s_locale.posix_tz[SETTINGS_POSIX_TZ_MAX_LEN] = '\0';
    snprintf(s_locale.tz_label, sizeof(s_locale.tz_label), "%s/%s", s_tz_zone_names[entry->zone],
             s_tz_city_names[entry->city]);
    settings_store_save_locale(&s_locale);

    // Auto-select the API region from the new time zone, unless the user
    // has manually pinned one (Settings > Time > Region) - see
    // docs/decisions/0009-regional-server-auto-selection.md.
    api_region_settings_t region_cfg;
    settings_store_load_api_region(&region_cfg);
    if (region_cfg.region_source == SETTINGS_API_REGION_SOURCE_AUTO)
    {
        apply_region_change(settings_api_region_from_tz_label(s_locale.tz_label), SETTINGS_API_REGION_SOURCE_AUTO);
    }

    // Applied immediately, not just on next boot.
    setenv("TZ", posix, 1);
    tzset();

    show_time_zone_cities(entry->zone); // rebuild to move the checkmark
}

static void time_zone_region_row_click_cb(lv_event_t *e)
{
    tz_zone_id_t zone = (tz_zone_id_t)(uintptr_t)lv_event_get_user_data(e);
    show_time_zone_cities(zone);
    show_settings_view(SETTINGS_VIEW_TIME_ZONE_CITIES);
}

static void build_time_zone_screen(lv_obj_t *screen)
{
    lv_obj_t *body =
        build_time_subscreen(screen, &s_time_zone_screen, "Time zones", NULL, NULL, time_zone_region_back_cb);

    // Alphabetizes both the region list below and every per-region city
    // list built later by show_time_zone_cities() (see tz_compare()). Only
    // needs to run once - this function itself only runs once, at startup.
    qsort(s_timezones, TZ_TABLE_COUNT, sizeof(s_timezones[0]), tz_compare);

    for (tz_zone_id_t zone = TZ_ZONE_AMERICA; zone < TZ_ZONE_COUNT; zone++)
    {
        if (tz_count_in_zone(zone) == 0)
        {
            continue;
        }
        build_nav_row(body, s_tz_zone_names[zone], time_zone_region_row_click_cb, (void *)(uintptr_t)zone);
    }
}

static void build_time_zone_city_screen(lv_obj_t *screen)
{
    s_time_zone_city_list = build_time_subscreen(screen, &s_time_zone_city_screen, "Time zones", "",
                                                  &s_time_zone_city_subtitle, time_zone_city_back_cb);
}

static void time_format_nav_row_click_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_TIME_FORMAT);
}

static void date_format_nav_row_click_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_DATE_FORMAT);
}

static void time_zones_nav_row_click_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_TIME_ZONES);
}

// Persists a new region/source pair (if it actually changed) and, when it
// did, forces the same full resync + WS reconnect a Wi-Fi gap resync uses -
// the underlying host changed, so any existing klines history came from
// the old one. See docs/decisions/0009-regional-server-auto-selection.md.
// When the resolved server itself changes (region_new != region_old, as
// opposed to just an AUTO/MANUAL source flip that resolves to the same
// server), every symbol's cached klines and per-symbol failure counters are
// also reset - a symbol marked "Unsupported" on the old server may well be
// valid on the new one, and stale history from the old host shouldn't
// linger on screen while the resync is in flight.
static void apply_region_change(settings_api_region_t region, settings_api_region_source_t source)
{
    api_region_settings_t cfg;
    settings_store_load_api_region(&cfg);
    if (cfg.region == region && cfg.region_source == source)
    {
        return;
    }
    bool region_changed = (cfg.region != region);
    cfg.region = region;
    cfg.region_source = source;
    settings_store_save_api_region(&cfg);
    if (region_changed)
    {
        app_state_reset_symbols_for_region_change();
    }
    app_state_sync_task_force_resync();
    app_state_notify_region_changed();
}

static void region_back_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_LOCALE);
}

static void region_refresh_marks(void)
{
    api_region_settings_t cfg;
    settings_store_load_api_region(&cfg);

    bool is_auto = (cfg.region_source == SETTINGS_API_REGION_SOURCE_AUTO);
    bool is_manual_intl = (!is_auto && cfg.region == SETTINGS_API_REGION_INTL);
    bool is_manual_us = (!is_auto && cfg.region == SETTINGS_API_REGION_US);

    lv_obj_t *checks[] = {s_region_check_auto, s_region_check_intl, s_region_check_us};
    bool shown[] = {is_auto, is_manual_intl, is_manual_us};
    for (size_t i = 0; i < 3; i++)
    {
        if (shown[i])
        {
            lv_obj_remove_flag(checks[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(checks[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // "Automatic" alone doesn't say which host that currently resolves to -
    // spell it out, since that's exactly the detail Phase 13 exists to get
    // right silently in the background.
    const char *resolved = (cfg.region == SETTINGS_API_REGION_US) ? "United States (Binance.US)"
                                                                    : "International (Binance.com)";
    lv_label_set_text_fmt(s_region_subtitle, "Currently: %s", resolved);
}

// user_data: 0 = Automatic, 1 = International, 2 = United States.
static void region_row_click_cb(lv_event_t *e)
{
    uintptr_t choice = (uintptr_t)lv_event_get_user_data(e);
    switch (choice)
    {
    case 0:
        apply_region_change(settings_api_region_from_tz_label(s_locale.tz_label), SETTINGS_API_REGION_SOURCE_AUTO);
        break;
    case 1:
        apply_region_change(SETTINGS_API_REGION_INTL, SETTINGS_API_REGION_SOURCE_MANUAL);
        break;
    default:
        apply_region_change(SETTINGS_API_REGION_US, SETTINGS_API_REGION_SOURCE_MANUAL);
        break;
    }
    region_refresh_marks();
}

static void region_nav_row_click_cb(lv_event_t *e)
{
    (void)e;
    show_settings_view(SETTINGS_VIEW_REGION);
}

static void build_region_screen(lv_obj_t *screen)
{
    lv_obj_t *body =
        build_time_subscreen(screen, &s_region_screen, "Region", "", &s_region_subtitle, region_back_cb);

    build_time_toggle_row(body, "Automatic", region_row_click_cb, (void *)(uintptr_t)0, &s_region_check_auto);
    build_time_toggle_row(body, "International (Binance.com)", region_row_click_cb, (void *)(uintptr_t)1,
                           &s_region_check_intl);
    build_time_toggle_row(body, "United States (Binance.US)", region_row_click_cb, (void *)(uintptr_t)2,
                           &s_region_check_us);

    region_refresh_marks();
}

static void build_locale_screen(lv_obj_t *screen)
{
    s_locale_screen = lv_obj_create(screen);
    lv_obj_remove_style_all(s_locale_screen);
    make_plain_container(s_locale_screen);
    lv_obj_set_size(s_locale_screen, LV_PCT(100), BOARD_JC4880P443C_LCD_V_RES - STATUSBAR_HEIGHT_PX);
    lv_obj_set_flex_flow(s_locale_screen, LV_FLEX_FLOW_COLUMN);

    build_subscreen_header(s_locale_screen, "Time", NULL, settings_back_cb);

    build_nav_row(s_locale_screen, "Time format", time_format_nav_row_click_cb, NULL);
    build_nav_row(s_locale_screen, "Date format", date_format_nav_row_click_cb, NULL);
    build_nav_row(s_locale_screen, "Time zones", time_zones_nav_row_click_cb, NULL);
    build_nav_row(s_locale_screen, "Region", region_nav_row_click_cb, NULL);
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

    settings_store_load_locale(&s_locale); // the Time screens below need it for their initial checkmarks

    build_settings_list(screen);
    build_locale_screen(screen);
    build_time_format_screen(screen);
    build_date_format_screen(screen);
    build_time_zone_screen(screen);
    build_time_zone_city_screen(screen);
    build_region_screen(screen);
    build_wifi_screen(screen);
    build_wifi_password_screen(screen);
    build_watchlist_manage_screen(screen);
    build_watchlist_add_screen(screen);
    build_updates_screen(screen);
    build_about_screen(screen);
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
        wifi_manager_pause_autoconnect(); // matches wifi_row_click_cb()
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
    else if (strcmp(target, "watchlist_manage") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        watchlist_manage_rebuild();
        show_settings_view(SETTINGS_VIEW_WATCHLIST_MANAGE);
    }
    else if (strcmp(target, "watchlist_add") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        watchlist_add_screen_reset();
        show_settings_view(SETTINGS_VIEW_WATCHLIST_ADD);

        // Mirrors the real screen-entry state - a tapped textarea would show
        // the keyboard too, and this screen reads as empty without it.
        lv_keyboard_set_textarea(s_watchlist_add_keyboard, s_watchlist_symbol_input);
        lv_obj_remove_flag(s_watchlist_add_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    else if (strcmp(target, "updates") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        updates_screen_reset();
        show_settings_view(SETTINGS_VIEW_UPDATES);
    }
    else if (strcmp(target, "about") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        show_settings_view(SETTINGS_VIEW_ABOUT);
    }
    else if (strcmp(target, "time") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        show_settings_view(SETTINGS_VIEW_LOCALE);
    }
    else if (strcmp(target, "time_format") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        show_settings_view(SETTINGS_VIEW_TIME_FORMAT);
    }
    else if (strcmp(target, "date_format") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        show_settings_view(SETTINGS_VIEW_DATE_FORMAT);
    }
    else if (strcmp(target, "time_zones") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        show_settings_view(SETTINGS_VIEW_TIME_ZONES);
    }
    else if (strcmp(target, "time_zone_cities") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        // argv[2] is a zone name (e.g. "Europe") - defaults to Europe (the
        // largest region) since that's the most useful case to inspect.
        const char *zone_name = (argc >= 3) ? argv[2] : "Europe";
        tz_zone_id_t zone = TZ_ZONE_EUROPE;
        for (size_t i = 0; i < TZ_ZONE_COUNT; i++)
        {
            if (strcmp(s_tz_zone_names[i], zone_name) == 0)
            {
                zone = (tz_zone_id_t)i;
                break;
            }
        }
        show_time_zone_cities(zone);
        show_settings_view(SETTINGS_VIEW_TIME_ZONE_CITIES);
    }
    else if (strcmp(target, "region") == 0)
    {
        set_active_screen(DISPLAY_UI_SCREEN_SETTINGS);
        show_settings_view(SETTINGS_VIEW_REGION);
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
        .help = "Jump directly to a screen: watchlist | settings | wifi | wifi_password [ssid] | "
                "watchlist_manage | watchlist_add | time | time_format | date_format | time_zones | "
                "time_zone_cities [zone] (dev builds only)",
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
