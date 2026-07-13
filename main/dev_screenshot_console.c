#include "dev_screenshot_console.h"

#include "sdkconfig.h"

#if CONFIG_DEV_SCREENSHOT_CONSOLE

#include "board_jc4880p443c.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "lvgl.h"
#include "mbedtls/base64.h"

#include <inttypes.h>
#include <stdio.h>

// A multiple of 3 so every chunk but (at most) the last base64-encodes with
// no '=' padding, keeping SCREENSHOT_DATA lines a uniform, predictable length.
#define SCREENSHOT_CHUNK_BYTES 768

static int cmd_screenshot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!board_jc4880p443c_display_lock(0))
    {
        printf("SCREENSHOT_ERROR reason=lock_failed\n");
        return 1;
    }

    uint32_t stride = lv_draw_buf_width_to_stride(BOARD_JC4880P443C_LCD_H_RES, LV_COLOR_FORMAT_RGB565);
    size_t buf_size = (size_t)stride * BOARD_JC4880P443C_LCD_V_RES;
    uint8_t *psram_buf = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, buf_size, MALLOC_CAP_SPIRAM);
    if (psram_buf == NULL)
    {
        board_jc4880p443c_display_unlock();
        printf("SCREENSHOT_ERROR reason=no_mem\n");
        return 1;
    }

    // lv_draw_buf_init() does not set LV_IMAGE_FLAGS_ALLOCATED, so LVGL
    // never tries to free/realloc this buffer - it's ours to free below
    // with heap_caps_free(), never lv_draw_buf_destroy().
    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, BOARD_JC4880P443C_LCD_H_RES, BOARD_JC4880P443C_LCD_V_RES, LV_COLOR_FORMAT_RGB565,
                     stride, psram_buf, buf_size);

    // lv_screen_active() is the one root screen; every sub-panel
    // (Watchlist/Settings/Wi-Fi/etc. in display_ui.c) is a child of it,
    // shown/hidden via a flag, so this always captures whatever is
    // currently visible with no per-page logic needed. The one exception is
    // a modal lv_msgbox (e.g. Settings > Display's night-mode time picker),
    // which LVGL parents to lv_layer_top() instead - a sibling of the
    // screen, not a descendant of it - so it would otherwise be invisible
    // to a snapshot rooted at lv_screen_active(). Prefer the top layer
    // whenever it actually has content.
    lv_obj_t *snapshot_root = lv_obj_get_child_count(lv_layer_top()) > 0 ? lv_layer_top() : lv_screen_active();
    lv_result_t res = lv_snapshot_take_to_draw_buf(snapshot_root, LV_COLOR_FORMAT_RGB565, &draw_buf);

    // Release the lock now, before the (slow) serial transfer below - it's
    // only needed while LVGL is actually being read into our buffer, not
    // while copying already-captured bytes out over serial.
    board_jc4880p443c_display_unlock();

    if (res != LV_RESULT_OK)
    {
        heap_caps_free(psram_buf);
        printf("SCREENSHOT_ERROR reason=snapshot_failed\n");
        return 1;
    }

    // Silence logging for the transfer so no interleaved ESP_LOGx line can
    // corrupt the framed reply below. This resets every tag's level, not
    // just this one's, so any tag-specific override in effect before this
    // call is lost on restore - acceptable for a dev-only tool.
    esp_log_level_t saved_level = esp_log_level_get(NULL);
    esp_log_level_set("*", ESP_LOG_NONE);

    uint32_t crc = esp_rom_crc32_le(0, psram_buf, buf_size);

    printf("SCREENSHOT_BEGIN width=%d height=%d stride=%" PRIu32 " format=RGB565 bytes=%" PRIu32 "\n",
           BOARD_JC4880P443C_LCD_H_RES, BOARD_JC4880P443C_LCD_V_RES, stride, (uint32_t)buf_size);

    char b64_chunk[((SCREENSHOT_CHUNK_BYTES + 2) / 3) * 4 + 1];
    size_t offset = 0;
    while (offset < buf_size)
    {
        size_t chunk_len = buf_size - offset;
        if (chunk_len > SCREENSHOT_CHUNK_BYTES)
        {
            chunk_len = SCREENSHOT_CHUNK_BYTES;
        }
        size_t olen = 0;
        mbedtls_base64_encode((unsigned char *)b64_chunk, sizeof(b64_chunk), &olen, psram_buf + offset, chunk_len);
        b64_chunk[olen] = '\0';
        fputs("SCREENSHOT_DATA ", stdout);
        fputs(b64_chunk, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        offset += chunk_len;
    }

    printf("SCREENSHOT_END crc32=%08" PRIx32 " bytes=%" PRIu32 "\n", crc, (uint32_t)buf_size);
    fflush(stdout);

    esp_log_level_set("*", saved_level);
    heap_caps_free(psram_buf);
    return 0;
}

esp_err_t dev_screenshot_console_register(void)
{
    const esp_console_cmd_t screenshot_cmd = {
        .command = "screenshot",
        .help = "Capture the current LVGL screen and stream it back as base64 (dev builds only)",
        .hint = NULL,
        .func = &cmd_screenshot,
    };
    return esp_console_cmd_register(&screenshot_cmd);
}

#else // !CONFIG_DEV_SCREENSHOT_CONSOLE

esp_err_t dev_screenshot_console_register(void) { return ESP_OK; }

#endif
