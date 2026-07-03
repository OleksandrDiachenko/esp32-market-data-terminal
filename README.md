# ESP32 Market Data Terminal

Embedded C / ESP-IDF firmware project for an ESP32-P4 based market data
terminal.

The project is currently in the bootstrap stage. The first goal is to build a
clean firmware foundation with a reproducible ESP-IDF setup, clear module
boundaries, tests for hardware-independent logic, and documented hardware
debugging steps.

## Hardware

- Target SoC: ESP32-P4
- ESP-IDF target: `esp32p4`
- Board/display: JC4880P443C_I_W, see [docs/hardware/jc4880p443c.md](docs/hardware/jc4880p443c.md)

## Toolchain

- ESP-IDF: 6.0.1
- Build system: ESP-IDF CMake + Ninja
- Language: C

## Build

Activate the ESP-IDF environment before building:

```sh
. /Users/siswood/.espressif/v6.0.1/esp-idf/export.sh
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
main/
  CMakeLists.txt
  esp32-market-data-terminal.c
```

Planned structure as the project grows:

```text
components/
docs/
test/
.github/
```

## Current Status

- ESP-IDF project skeleton exists
- Target is configured for ESP32-P4
- Firmware builds successfully
- Application currently logs a startup message

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
- No local configuration or generated files are committed
- Error paths are considered for firmware changes
- Hardware test notes are added when hardware behavior changes
- Documentation is updated when setup or behavior changes

## Notes

This is a firmware architecture and reliability project. Public market data APIs
may be used later as demo data sources, but private trading APIs, credentials,
and secrets must not be committed.
