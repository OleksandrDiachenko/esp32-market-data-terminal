---
name: dev-screenshot
description: Capture a screenshot of the device's current LVGL screen over serial so Claude Code can actually see the on-device UI, instead of only reading main/display_ui.c source or logs. Dev-only - requires a local build flag; must never be enabled in production/release builds.
---

# Device screenshot over serial (dev-only)

Lets Claude Code trigger an on-device screenshot by sending a command over the same serial
port already used for flashing/logs (`/dev/cu.usbmodem101`), and decode the reply into a real
PNG file that can be viewed with the `Read` tool. Useful for visually verifying UI changes on
real hardware instead of only reasoning about `main/display_ui.c` source.

See `docs/validation/serial-console-usb-jtag-rebind-hardware-test.md` for how the underlying
serial-input path was validated, and `main/dev_screenshot_console.c` for the exact wire format.

## Prerequisite: enable the build flag (one-time, local only)

The `screenshot` console command is compiled out by default. Enable it in your **local**
sdkconfig (gitignored - never add this to `sdkconfig.defaults`):

```sh
idf.py menuconfig   # Development tools -> Enable 'screenshot' console command
# or edit sdkconfig directly: CONFIG_DEV_SCREENSHOT_CONSOLE=y
idf.py reconfigure  # required after a manual sdkconfig edit, to propagate the
                     # `select LV_USE_SNAPSHOT` dependency
idf.py -p /dev/cu.usbmodem101 flash
```

Before shipping a release build, confirm this flag is off (a clean `sdkconfig` derived only
from `sdkconfig.defaults` has it off by default).

## Usage

```sh
python3 tools/dev_screenshot.py --port /dev/cu.usbmodem101 --out <path>.png
```

First-time host setup: `pip install -r tools/requirements.txt` (pyserial, Pillow, numpy - not
otherwise used by this project, so nothing else pulls them in).

The script prints the saved PNG path on success - `Read` that path to see the current
on-device UI. It captures whatever is currently visible (`lv_screen_active()`); every
sub-panel in `main/display_ui.c` (Watchlist, Settings, Wi-Fi, etc.) is a child of the one root
screen, shown/hidden via a flag, so this works on any screen without special-casing - just
navigate the physical device to the screen you want captured first.

### Navigating without a physical tap

Add `--nav TARGET` to jump to a screen before capturing, instead of asking someone to tap the
device by hand:

```sh
python3 tools/dev_screenshot.py --port /dev/cu.usbmodem101 --nav wifi --out wifi_list.png
python3 tools/dev_screenshot.py --port /dev/cu.usbmodem101 --nav wifi_password --ssid "MyHomeNet" --out wifi_pw.png
```

`--nav` accepts `watchlist`, `settings`, `wifi`, or `wifi_password` (pair with `--ssid` to set
the title shown on the password screen - purely cosmetic, it doesn't need to match a real
nearby network). This sends the `nav` console command registered by
`display_ui_register_dev_nav_console()` in `main/display_ui.c`, which reuses the same
`set_active_screen()`/`show_settings_view()` functions the real touch handlers call, gated by
the same `CONFIG_DEV_SCREENSHOT_CONSOLE` flag as `screenshot` - no separate build flag to
enable. `nav` only drives screen navigation, not touch simulation, so anything that needs an
actual tap (typing on the keyboard, toggling a switch) still needs the physical device.

## Known side effect

Enabling this flag doesn't just add `screenshot` - the console REPL itself
(`main/ota_console.c`) was rebound from UART0 to USB-Serial-JTAG as a prerequisite (this part
*is* permanent, in `sdkconfig.defaults`, not gated by the dev flag). As a result,
`ota_check`/`ota_update` are also reachable from `/dev/cu.usbmodem101` now, which they
previously weren't.

## Troubleshooting

- **Timeout, never sees `SCREENSHOT_BEGIN`**: the build wasn't compiled with
  `CONFIG_DEV_SCREENSHOT_CONSOLE=y`, or the port is wrong.
- **"Guru Meditation Error... Stack protection fault" in task `console_repl`**: this was hit
  during development - `lv_snapshot_take_to_draw_buf()` runs LVGL's full render pipeline
  synchronously on the calling task's stack, and the REPL task's default 4096-byte stack isn't
  enough (LVGL's own dedicated render task uses 7168 for the same call chain). Already fixed
  in `main/ota_console.c` by bumping `repl_config.task_stack_size` to 12288 whenever
  `CONFIG_DEV_SCREENSHOT_CONSOLE` is on - if you see this again, that number may need to grow
  further for a more complex screen.
- **CRC32 mismatch warning**: the script still writes the PNG (useful for a partial/dev look),
  but treat the image as possibly corrupted; a transfer-speed/log-interleaving issue would show
  up this way.
