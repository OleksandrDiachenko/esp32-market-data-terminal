# Market Data Ticker

Embedded C / ESP-IDF firmware for Market Data Ticker: an ESP32-P4-based
touchscreen device that shows a live cryptocurrency watchlist (price, change,
sparkline) sourced from the public Binance REST and WebSocket APIs.

Built as a portfolio project for practicing a professional embedded
firmware workflow: clear module boundaries, host-side tests for
hardware-independent logic, real hardware validation, and small reviewed
changes tracked in [docs/roadmap.md](docs/roadmap.md) and
[docs/decisions/](docs/decisions/).

## Features

- Live watchlist (up to 10 symbols) with price, 24h change, and sparkline,
  updated via a Binance WebSocket stream and periodic REST sync
- Wi-Fi setup and management (scan, connect, saved profiles) from the device UI
- Locale settings: 12h/24h time, date format, time zone (region/city picker)
- Regional API server auto-selection (Binance.com vs. Binance.US) derived
  from the selected time zone, with manual override
  (see [docs/decisions/0009-regional-server-auto-selection.md](docs/decisions/0009-regional-server-auto-selection.md))
- Over-the-air firmware updates via GitHub Releases
  (see [docs/decisions/0006-ota-firmware-update.md](docs/decisions/0006-ota-firmware-update.md))
- Configurable display brightness and scheduled night mode

## Hardware

- Target SoC: ESP32-P4
- ESP-IDF target: `esp32p4`
- Board/display: JC4880P443C_I_W, see [docs/hardware/jc4880p443c.md](docs/hardware/jc4880p443c.md)

## Toolchain

- ESP-IDF: 6.0.2
- Build system: ESP-IDF CMake + Ninja
- Language: C

## Build

Activate the ESP-IDF environment before building:

```sh
. "$HOME/.espressif/v6.0.2/esp-idf/export.sh"
idf.py build
```

The project target is stored in `sdkconfig.defaults`:

```text
CONFIG_IDF_TARGET="esp32p4"
CONFIG_IDF_TARGET_ESP32P4=y
```

## Flash And Monitor

Connect the board and run:

```sh
idf.py -p <PORT> flash monitor
```

Example port names:

```text
/dev/cu.usbmodem*
/dev/cu.usbserial*
```

## Project Structure

```text
CMakeLists.txt
sdkconfig.defaults
main/                   - app entry point, LVGL display UI, dev console
components/
  app_state             - runtime state model + sync/WS/OTA background tasks
  board_jc4880p443c     - display/touch/backlight board bring-up
  display_format        - price/number formatting helpers
  market_data_client    - Binance REST client (exchangeInfo, klines)
  market_data_ws_client - Binance WebSocket kline stream client
  ota_client            - GitHub Releases OTA update client
  settings_store        - persisted settings (display/symbols/locale/
                          api region), sealed-blob NVS codec
  time_sync             - SNTP time sync
  wifi_manager          - Wi-Fi connection policy + profile store
docs/                   - architecture, roadmap, ADRs, hardware/testing notes
.github/                - CI workflows
```

## Current Status

See [docs/roadmap.md](docs/roadmap.md) for the phase-by-phase build log.
The firmware currently boots to a live watchlist dashboard with Wi-Fi,
locale/timezone, watchlist, and OTA update settings screens, backed by
host-tested settings/parsing logic and validated on real hardware.

## Development Workflow

Recommended branch model:

```text
main        - stable baseline
feature/*   - new functionality
bugfix/*    - bug fixes
chore/*     - tooling and maintenance
docs/*      - documentation
test/*      - tests
```

Before opening a pull request:

- Build passes with `idf.py build`
- Host tests pass (commands are listed in [docs/testing.md](docs/testing.md))
- Repo-wide clang-format and first-party cppcheck gates pass in CI
- No local configuration or generated files are committed
- Error paths are considered for firmware changes
- Hardware test notes are added when hardware behavior changes
- Documentation is updated when setup or behavior changes

## License & data usage

The **source code** of this project is licensed under the
[Apache License 2.0](LICENSE) — free to use, modify, and distribute,
including a patent grant.

**Market data** shown on the device is provided by the public
[Binance](https://www.binance.com) APIs and is **separate from the code
license**. Under Binance's Terms of Use this data may be used for
**non-commercial, informational purposes only** — you may not charge for it
or profit from it (ads, referral fees). This is a free, non-commercial
open-source build, which satisfies that restriction.

Market data may be delayed, incomplete, or inaccurate. Nothing shown on the
device is financial, investment, or trading advice. Always verify prices on
the official exchange.

## Notes

This is a firmware architecture and reliability project. Only public market
data APIs are used (no keys/secrets); private trading APIs, credentials, and
secrets must not be committed.
