# Setup

## Requirements

- ESP-IDF 6.0.2
- JC4880P443C_I_W board
- USB data connection to the ESP32-P4 port

The dependency versions are pinned in `dependencies.lock`; do not hand-edit
`managed_components/`.

## Build

```sh
. "$HOME/.espressif/v6.0.2/esp-idf/export.sh"
idf.py build
```

`sdkconfig` is generated locally from the tracked `sdkconfig.defaults` and is
intentionally ignored. To verify a clean configuration, remove the local
`sdkconfig` only when you intentionally want to regenerate it, then run
`idf.py reconfigure`.

## Flash and monitor

```sh
idf.py -p <PORT> flash monitor
```

Typical macOS ports are `/dev/cu.usbmodem*` and `/dev/cu.usbserial*`.

## Host tests

Run all commands listed in [testing.md](testing.md). They use the host C
compiler with AddressSanitizer and UndefinedBehaviorSanitizer; they do not
require attached hardware.

## Development-only builds

`CONFIG_DEV_SCREENSHOT_CONSOLE` and `CONFIG_UI_DIAGNOSTICS` are off in the
release defaults. Enable them only in the ignored local `sdkconfig` through
`idf.py menuconfig`, then run `idf.py reconfigure`. Before release validation,
build once from clean defaults and confirm both options are disabled.
