# Hardware validation: staged black frame before dashboard boot

## Environment

- Date: 2026-07-13
- Board: JC4880P443C_I_W
- Target: esp32p4 (revision v1.3)
- ESP-IDF: v6.0.2
- Port: `/dev/cu.usbmodem101`
- Firmware commit: `d67604e` (`fix: stage black frame before dashboard render`)
- LVGL pool: 512 KB in PSRAM; display buffering and rotation unchanged

## Baseline

`5181b31` already removed the prolonged full-screen white boot state, but
the first cold boot still briefly showed a white horizontal strip matching
the 40 px status bar height.

## Change

The UI first flushes the existing black root screen, waits for the current
scanout guard, and enables the configured backlight. It then builds the
PSRAM-backed dashboard widgets. This makes the boot-critical visible frame
independent of the slower first dashboard allocation/render.

The LVGL allocator, pool size, framebuffer count, `direct_mode`,
`avoid_tearing`, display timings, and rotation were not changed.

## Command

```sh
idf.py -B /tmp/market-terminal-5181b31/build \
  -D SDKCONFIG=/tmp/market-terminal-5181b31/sdkconfig build
idf.py -B /tmp/market-terminal-5181b31/build -p /dev/cu.usbmodem101 flash
```

## Result

**Passed.** On the real board, the user confirmed:

- no prolonged full-screen white boot state;
- no transient white 40 px strip at the bottom;
- dashboard UI, touch, and Wi-Fi operate normally after boot.

