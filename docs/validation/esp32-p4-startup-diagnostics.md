# Debug report: ESP32-P4 startup diagnostics

## Symptom

Initial ESP32-P4 firmware bring-up had flash and boot compatibility issues.

## Environment

- Board: JC4880P443C_I_W
- MCU: ESP32-P4
- ESP-IDF version: 6.0.1
- Branch: feature/startup-diagnostics
- Flash: 16 MB BOYA
- PSRAM: 32 MB
- Flash method: UART
- Port: /dev/tty.usbmodem101

## Fix

Added ESP32-P4 board baseline in `sdkconfig.defaults`:

- early ESP32-P4 silicon support
- 16 MB flash size
- BOYA flash support
- PSRAM 200 MHz
- 360 MHz CPU frequency
- bootloader log level reduced to keep bootloader within partition offset

Added startup diagnostics for:

- chip model check & logging
- chip revision logging
- chip features logging
- chip core count logging
- internal heap minimum check

## Validation

Firmware builds successfully with:

```sh
idf.py build
idf.py -p /dev/tty.usbmodem101 flash monitor
```

Relevant logs:

```text
I (1196) market_terminal: ESP32 Market Data Terminal started
I (1206) startup_diag: Performing startup diagnostics...
I (1206) startup_diag: Chip model diagnostics passed, model: ESP32-P4
I (1216) startup_diag: Chip revision: 103
I (1216) startup_diag: Chip features: 0x00000000
I (1226) startup_diag: Chip cores: 2
I (1226) startup_diag: Internal heap diagnostics passed, free heap: 445679
I (1236) startup_diag: Startup diagnostics completed.
```

## Result

Startup diagnostics pass on real ESP32-P4 hardware.

## Follow-up

- Revisit internal heap threshold after display and network modules are added.
- Add runtime diagnostics after the main application tasks are introduced.