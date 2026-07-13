#include "board_jc4880p443c.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include <string.h>

static const char *TAG = "board_jc4880p443c";

enum
{
    BOARD_LCD_BITS_PER_PIXEL = 24,

    BOARD_LCD_BACKLIGHT_GPIO = GPIO_NUM_23,
    BOARD_LCD_RESET_GPIO = GPIO_NUM_5,
    BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL = 1,
    BOARD_LCD_BRIGHTNESS_LEDC_TIMER = 1,

    BOARD_MIPI_DSI_PHY_LDO_CHANNEL = 3,
    BOARD_MIPI_DSI_PHY_LDO_VOLTAGE_MV = 2500,
    BOARD_MIPI_DSI_LANE_NUM = 2,
    BOARD_MIPI_DSI_LANE_BITRATE_MBPS = 500,

    BOARD_TOUCH_I2C_PORT = 1,
    BOARD_TOUCH_I2C_SDA_GPIO = GPIO_NUM_7,
    BOARD_TOUCH_I2C_SCL_GPIO = GPIO_NUM_8,
    BOARD_TOUCH_I2C_CLK_SPEED_HZ = 400000,
    BOARD_TOUCH_RESET_GPIO = GPIO_NUM_NC,
    BOARD_TOUCH_INT_GPIO = GPIO_NUM_NC,

    BOARD_LCD_DRAW_BUFF_SIZE = BOARD_JC4880P443C_LCD_H_RES * 40,
};

// Board-specific ST7701S init sequence. The registry esp_lcd_st7701
// component's default sequence does not bring up this panel; these register
// writes come from this board's verified working configuration.
static const st7701_lcd_init_cmd_t st7701_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71},
     16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D},
     16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0},
     16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0},
     16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B},
     16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 20},
};

static i2c_master_bus_handle_t touch_i2c_handle;

static esp_err_t backlight_pwm_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BOARD_LCD_BRIGHTNESS_LEDC_TIMER,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BOARD_LCD_BRIGHTNESS_LEDC_TIMER,
        // Off until the app explicitly lights it (display_ui_start(), after
        // the first real frame has been flushed to the panel). duty = 1023
        // here lit the backlight to 100% as a side effect of LEDC channel
        // init - seconds before any content existed - which is what made the
        // boot white-screen survive every attempt to fix it by reordering
        // the *later* backlight calls.
        .duty = 0,
        .hpoint = 0,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "configure backlight LEDC timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "configure backlight LEDC channel");
    return ESP_OK;
}

static esp_err_t dsi_phy_power_on(void)
{
    static esp_ldo_channel_handle_t ldo_handle;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BOARD_MIPI_DSI_PHY_LDO_CHANNEL,
        .voltage_mv = BOARD_MIPI_DSI_PHY_LDO_VOLTAGE_MV,
    };
    return esp_ldo_acquire_channel(&ldo_cfg, &ldo_handle);
}

static esp_lcd_dpi_panel_config_t make_dpi_panel_config(void)
{
    return (esp_lcd_dpi_panel_config_t){
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 34,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_JC4880P443C_LCD_H_RES,
            .v_size = BOARD_JC4880P443C_LCD_V_RES,
            .hsync_back_porch = 42,
            .hsync_pulse_width = 12,
            .hsync_front_porch = 42,
            .vsync_back_porch = 8,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 166,
        },
    };
}

static esp_err_t panel_new(esp_lcd_panel_handle_t *out_panel, esp_lcd_panel_io_handle_t *out_io)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    ESP_RETURN_ON_ERROR(dsi_phy_power_on(), TAG, "power MIPI DSI PHY");

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BOARD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BOARD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus), err, TAG, "create MIPI DSI bus");

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &io), err, TAG, "create DBI panel IO");

    esp_lcd_dpi_panel_config_t dpi_config = make_dpi_panel_config();
    st7701_vendor_config_t vendor_config = {
        .init_cmds = st7701_init_cmds,
        .init_cmds_size = sizeof(st7701_init_cmds) / sizeof(st7701_init_cmds[0]),
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
        },
        .flags = {
            .use_mipi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = BOARD_LCD_BITS_PER_PIXEL,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = BOARD_LCD_RESET_GPIO,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_config, &panel), err, TAG, "create ST7701 panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), err, TAG, "reset ST7701 panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), err, TAG, "initialize ST7701 panel");

    *out_panel = panel;
    *out_io = io;
    return ESP_OK;

err:
    if (panel != NULL)
    {
        esp_lcd_panel_del(panel);
    }
    if (io != NULL)
    {
        esp_lcd_panel_io_del(io);
    }
    if (dsi_bus != NULL)
    {
        esp_lcd_del_dsi_bus(dsi_bus);
    }
    return ret;
}

static esp_err_t touch_i2c_ensure_init(void)
{
    if (touch_i2c_handle != NULL)
    {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_I2C_SDA_GPIO,
        .scl_io_num = BOARD_TOUCH_I2C_SCL_GPIO,
    };
    return i2c_new_master_bus(&bus_config, &touch_i2c_handle);
}

esp_err_t board_jc4880p443c_display_start(lv_display_t **out_display)
{
    if (out_display == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(backlight_pwm_init(), TAG, "initialize backlight PWM");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(panel_new(&panel, &io), TAG, "bring up ST7701 panel");
    ESP_LOGI(TAG, "ST7701 panel initialized: %dx%d", BOARD_JC4880P443C_LCD_H_RES, BOARD_JC4880P443C_LCD_V_RES);

    // The DPI peripheral starts scanning this framebuffer out to the panel
    // continuously from here on, and nothing guarantees its initial content
    // (or anything a pre-dashboard LVGL flush later leaves in it) is dark.
    // Clear it to black so no boot-time frame can ever show light pixels -
    // the backlight sequencing in display_ui_start() keeps the panel unlit
    // until the first real frame, and this closes the remaining gap where a
    // region the first flush hadn't covered yet (e.g. the status-bar-sized
    // final render stripe) still held stale light content when the
    // backlight came on.
    void *fb0 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0), TAG, "get DPI framebuffer");
    const size_t fb_bytes =
        (size_t)BOARD_JC4880P443C_LCD_H_RES * BOARD_JC4880P443C_LCD_V_RES * (BOARD_LCD_BITS_PER_PIXEL / 8);
    memset(fb0, 0, fb_bytes);
    // The framebuffer lives in PSRAM behind the CPU data cache while the DPI
    // engine reads physical memory - write the zeros back or the panel keeps
    // scanning whatever was there before.
    ESP_RETURN_ON_ERROR(esp_cache_msync(fb0, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG,
                         "sync cleared framebuffer");

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "initialize LVGL port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_DRAW_BUFF_SIZE,
        .double_buffer = false,
        .hres = BOARD_JC4880P443C_LCD_H_RES,
        .vres = BOARD_JC4880P443C_LCD_V_RES,
        .monochrome = false,
        // NOTE: with .flags.sw_rotate = true below, esp_lvgl_port never calls
        // esp_lcd_panel_mirror()/swap_xy() at all (see
        // lvgl_port_disp_rotation_update() in esp_lvgl_port_disp.c, which
        // returns immediately when sw_rotate is set) - this static rotation
        // struct is therefore dead when sw_rotate is on. 180-degree rotation
        // is instead applied at runtime via lv_display_set_rotation() below,
        // which drives the sw_rotate code path (lv_draw_sw_rotate) that
        // actually flips the rendered pixel buffer.
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB888,
#endif
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
            .sw_rotate = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };

    lv_display_t *display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (display == NULL)
    {
        ESP_LOGE(TAG, "LVGL DSI display registration failed");
        return ESP_FAIL;
    }

    // 180-degree rotation (sw_rotate path - see the disp_cfg.rotation
    // comment above). LV_DISPLAY_ROTATION_180 doesn't swap hres/vres, so no
    // other sizing changes are needed. Touch coordinates need no matching
    // change here - see the touch_config.flags comment in
    // board_jc4880p443c_touch_start() below.
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_180);

    // From this point the LVGL port task is free to render and flush the
    // default screen at any moment - and LVGL's default theme is light. Make
    // the not-yet-populated screen black so any flush that happens before
    // display_ui builds the real UI paints black over the (also black -
    // cleared above) framebuffer, never a light frame. display_ui restyles
    // this same screen with its own palette when it builds the dashboard.
    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(display);
        if (scr != NULL)
        {
            lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        }
        lvgl_port_unlock();
    }

    *out_display = display;
    return ESP_OK;
}

esp_err_t board_jc4880p443c_touch_start(lv_display_t *display, lv_indev_t **out_indev)
{
    if (display == NULL || out_indev == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(touch_i2c_ensure_init(), TAG, "initialize touch I2C bus");

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.scl_speed_hz = BOARD_TOUCH_I2C_CLK_SPEED_HZ;

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(touch_i2c_handle, &io_config, &io_handle), TAG,
                         "create GT911 panel IO");

    esp_lcd_touch_config_t touch_config = {
        .x_max = BOARD_JC4880P443C_LCD_H_RES,
        .y_max = BOARD_JC4880P443C_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_RESET_GPIO,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        // NOTE: leave these at 0/0/0, even though the display above is
        // rotated 180 degrees. lvgl_port_touchpad_read() hands LVGL raw
        // touch-panel coordinates, and LVGL's own indev pipeline
        // (lv_indev.c:743 -> lv_display_rotate_point(), see lv_display.c)
        // already re-maps pointer input for the display's current
        // lv_display_get_rotation() - for ROTATION_180 that's exactly
        // `x' = hor_res - x - 1, y' = ver_res - y - 1`. Mirroring here too
        // would apply that same correction twice and cancel it out (this
        // was tried and confirmed on hardware: touch behaved as if the
        // display were never rotated).
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io_handle, &touch_config, &touch), TAG, "initialize GT911 touch");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = display,
        .handle = touch,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
    if (indev == NULL)
    {
        ESP_LOGE(TAG, "LVGL touch input registration failed");
        return ESP_FAIL;
    }

    *out_indev = indev;
    return ESP_OK;
}

esp_err_t board_jc4880p443c_backlight_early_off(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BACKLIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure backlight GPIO");
    return gpio_set_level(BOARD_LCD_BACKLIGHT_GPIO, 0);
}

esp_err_t board_jc4880p443c_backlight_on(void)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL, 1023), TAG,
                         "set backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL);
}

esp_err_t board_jc4880p443c_backlight_off(void)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL, 0), TAG,
                         "clear backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL);
}

esp_err_t board_jc4880p443c_backlight_set_percent(uint8_t percent)
{
    if (percent > 100)
    {
        percent = 100;
    }
    uint32_t duty = ((uint32_t)percent * 1023) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL, duty), TAG,
                         "set backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BOARD_LCD_BRIGHTNESS_LEDC_CHANNEL);
}

bool board_jc4880p443c_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void board_jc4880p443c_display_unlock(void)
{
    lvgl_port_unlock();
}
