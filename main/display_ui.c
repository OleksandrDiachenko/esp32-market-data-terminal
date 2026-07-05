#include "display_ui.h"

#include "app_state.h"
#include "board_jc4880p443c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>

static const char *TAG = "display_ui";

#define ROW_HEIGHT_PX 76
#define ROW_SIDE_COL_WIDTH_PX 128
#define UPDATE_PERIOD_MS 1000

#define COLOR_INK lv_color_hex(0x0A0C10)
#define COLOR_TEXT lv_color_hex(0xECEEF2)
#define COLOR_MUTED lv_color_hex(0x6E7686)
#define COLOR_UP lv_color_hex(0x2FD481)
#define COLOR_DOWN lv_color_hex(0xFF5D6C)
#define COLOR_WARN lv_color_hex(0xF2A93C)
#define COLOR_HAIRLINE lv_color_hex(0x1B1F26)

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
}

static void display_ui_render(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COLOR_INK, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, COLOR_TEXT, 0);

    s_rows_container = lv_obj_create(screen);
    lv_obj_remove_style_all(s_rows_container);
    lv_obj_set_size(s_rows_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_rows_container, LV_FLEX_FLOW_COLUMN);

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
