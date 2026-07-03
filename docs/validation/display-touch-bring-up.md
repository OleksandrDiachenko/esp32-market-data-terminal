# Hardware validation: Display and touch bring-up (Phase 4)

## Environment

- Date: 2026-07-03
- Board: JC4880P443C_I_W
- Target: esp32p4
- ESP-IDF: v6.0.1
- Port: /dev/cu.usbmodem101

## Command

```sh
idf.py -p /dev/cu.usbmodem101 flash monitor
```

## Expected result

- ST7701 MIPI DSI panel initializes without error.
- GT911 touch controller is detected on I2C and registered as an LVGL input
  device.
- LVGL renders a centered label on screen.
- No DSI underrun, DMA2D assert, or LVGL buffer errors in the log.

## Observed logs

```text
I (1136) st7701: version: 2.0.2
I (1166) st7701_mipi: LCD ID: FF FF FF
I (1166) st7701_mipi:  st7701->madctl_val: 0x0, st7701->colmod_val: 0x77
I (1306) board_jc4880p443c: ST7701 panel initialized: 480x800
I (1306) LVGL: Starting LVGL task
I (1306) GT911: I2C address initialization procedure skipped - using default GT9xx setup
I (1306) GT911: TouchPad_ID:0x39,0x31,0x31
I (1306) GT911: TouchPad_Config_Version:250
I (1326) display_ui: Display UI started.
I (1326) main_task: Returned from app_main()
```

## Result

Passed. First attempt failed with
`lvgl_port_add_disp_priv(314): DMA buffer can be used only in display color
format RGB565 (not aligned copy)!` using `buff_dma = true` /
`buff_spiram = false` with RGB888 — fixed by switching to
`buff_dma = false` / `buff_spiram = true`, matching the confirmed-working
config this board layer was based on. See
`docs/hardware/jc4880p443c.md` for the corrected LVGL buffer guidance.

Visually confirmed on the physical panel: "ESP32 Market Data Terminal" label
renders as black text on a white background, centered.

## Follow-up

- Touch input is registered but not yet exercised with an actual touch event
  in this test.
