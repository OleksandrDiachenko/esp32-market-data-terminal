# Device screenshots

The dev-only serial workflow captures the active LVGL screen from real
hardware into a PNG. Firmware support is compiled only when the ignored local
`sdkconfig` enables `CONFIG_DEV_SCREENSHOT_CONSOLE`; release defaults keep it
off.

## Setup

```sh
idf.py menuconfig
# Development tools -> Enable screenshot console command
idf.py reconfigure
idf.py -p /dev/cu.usbmodem101 flash
python3 -m pip install -r tools/requirements.txt
```

## Capture

```sh
python3 tools/dev_screenshot.py \
  --port /dev/cu.usbmodem101 \
  --nav watchlist \
  --out /tmp/watchlist.png
```

Supported navigation targets:

```text
watchlist settings wifi wifi_password watchlist_manage watchlist_add
time time_format date_format time_zones time_zone_cities region display
night_time updates about disclaimer
```

Use `--ssid <name>` with `wifi_password`. Navigation calls the same screen
lifecycle functions as touch handlers, but it does not simulate typing,
dragging, switching controls or OTA confirmation.

The firmware sends RGB565 snapshot chunks with length and CRC32. A timeout
usually means the dev option is disabled or the serial port is wrong. A CRC
mismatch makes the image invalid as release evidence even if a partial PNG is
written.

The console implementation is in `main/dev_console.c` and
`main/dev_screenshot_console.c`; there is no `ota_console.c` in the current
architecture.
