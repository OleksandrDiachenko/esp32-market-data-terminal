# Hardware validation: esptool-only first flash (README beginner path)

## Environment

- Date: 2026-07-14
- Board: JC4880P443C_I_W (ESP32-P4 + ESP32-C6 module `JC-ESP32P4-M3`)
- Host target: esp32p4, ESP-IDF v6.0.2
- esptool: v5.3.1 (bundled with the ESP-IDF v6.0.2 install)
- Port: `/dev/cu.usbmodem101`

## Method

Ran the exact commands documented in the README's "Flashing a pre-built
release" section and `docs/decisions/0006-ota-firmware-update.md`'s "Cutting
a release" note, to confirm they work as written rather than assumed.

```sh
idf.py build
cd build && esptool --chip esp32p4 merge-bin -o ../crypto-market-data-ticker-factory.bin @flash_args
cd ..
esptool --chip esp32p4 -p /dev/cu.usbmodem101 -b 460800 write-flash 0x0 crypto-market-data-ticker-factory.bin
```

## Findings and fixes to the documented commands

Three corrections were needed versus the first draft of the instructions,
all now reflected in the README/ADR text above:

1. **`merge_bin` is deprecated in esptool v5.3.1** - it still works but
   prints a deprecation warning; the current subcommand name is
   `merge-bin`.
2. **`@flash_args` must be run from inside `build/`** - the paths it
   contains (`bootloader/bootloader.bin`, `partition_table/partition-table.bin`,
   `ota_data_initial.bin`, `crypto-market-data-ticker.bin`) are relative to
   that directory; running the command from the project root failed with
   `No such file or directory: 'bootloader/bootloader.bin'`.
3. **`esptool.py` (with the `.py` suffix) is itself a deprecated wrapper** -
   it works but prints `Warning: DEPRECATED: 'esptool.py' is deprecated.
   Please use 'esptool' instead` on every invocation, which reads as an
   error to a first-time user. Both the README and the ADR now use the
   bare `esptool` entry point pip installs, which prints no warning.

`@flash_args` itself already lists all four components a from-scratch flash
needs (bootloader at `0x2000`, partition table at `0x8000`, `otadata` at
`0x10000`, app at `0x20000`) - no manual offset list was needed for the
merge step.

## Result

**Passed.** `merge-bin` produced a 1,981,072-byte single-file image
(`crypto-market-data-ticker-factory.bin`). `write-flash 0x0` wrote and
verified it against the board in ~17s. Boot log (captured via a pyserial
DTR/RTS reset + read, since `idf.py monitor` needs a real TTY) shows a clean
boot indistinguishable from a normal `idf.py flash`:

```text
I (1106) app_init: Project name:     crypto-market-data-ticker
I (1106) app_init: App version:      0.10.1
I (1106) app_init: ESP-IDF:          v6.0.2
...
I (1405) board_jc4880p443c: ST7701 panel initialized: 480x800
I (1525) GT911: TouchPad_ID:0x39,0x31,0x31
I (3055) display_ui: Display UI started.
I (5465) wifi_manager: ESP32-C6 co-processor firmware: 2.12.9
I (5485) wifi_manager_slave_ota: Co-processor firmware already up to date
I (5545) wifi_manager: Connecting to 'TV' (origin=1)
I (5765) RPC_WRAP: ESP Event: Station mode: Connected
I (6785) esp_netif_handlers: sta ip: 192.168.100.86, mask: 255.255.255.0, gw: 192.168.100.1
I (8105) time_sync: System time synced via SNTP
```

Display/touch init, ESP32-C6 co-processor link, and the previously-saved
Wi-Fi profile all came up correctly - confirming the merged image (which
only covers the bootloader/partition-table/otadata/app regions, not
`nvs`/`wifi_cfg`/`slave_fw`) is non-destructive to existing Wi-Fi
credentials and co-processor firmware, same property `idf.py flash` already
has.

## Follow-up

None - the two fixes above are already folded into the README and ADR 0006
text; this report exists to record that they were hardware-verified, not
just written from reading `idf.py`'s own printed flash command.
