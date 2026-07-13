# Testing

## Firmware builds

CI performs a clean ESP-IDF 6.0.2 build for `esp32p4`. Locally:

```sh
. "$HOME/.espressif/v6.0.2/esp-idf/export.sh"
idf.py build
idf.py size
idf.py size-components
```

Development-only builds may enable `CONFIG_DEV_SCREENSHOT_CONSOLE` and
`CONFIG_UI_DIAGNOSTICS` in the ignored local `sdkconfig`. Release validation
must also build from `sdkconfig.defaults`, where both are off.

## Host tests

Each command uses `-Wall -Wextra -Werror` plus ASan/UBSan:

```sh
make -C components/wifi_manager/host_test test
make -C components/settings_store/host_test test
make -C components/market_data_client/host_test test
make -C components/market_data_ws_client/host_test test
make -C components/app_state/host_test test
make -C components/ota_client/host_test test
make -C components/display_format/host_test test
make -C main/host_test test
```

Coverage focuses on pure logic: Wi-Fi policy/profile codecs, persisted
settings and empty-state validation, REST/WebSocket parsers and builders,
retry/kline merge logic, OTA JSON, display formatting, symbol sanitizing,
RSSI mapping, relative time and scheduled night windows.

## Hardware-bound code

The following is validated through firmware builds, static analysis and the
manual reports under [validation/](validation/): real NVS I/O, ESP-Hosted
Wi-Fi, SNTP/TLS/HTTP/WebSocket transport, FreeRTOS queue/task orchestration,
OTA partitions/rollback, LVGL rendering/touch and board drivers. These paths
are not disguised as host unit tests with incomplete platform mocks.

## Static analysis and formatting

- The blocking `cppcheck` job scans pure modules with style checks.
- After the ESP-IDF build creates `build/compile_commands.json`,
  `tools/run_first_party_cppcheck.sh` scans all first-party `main/` and
  `components/` translation units for warnings, performance and portability
  findings while excluding framework/vendor code.
- The blocking `format-check` job runs repo-wide `clang-format --dry-run
  --Werror` over first-party C headers and sources.
- Compiler warnings from first-party code are treated as review findings;
  vendored dependencies are not forced under a repo-wide `-Werror` policy.

## Manual release regression

Before a release, record boot/first-entry timings, every screenshot target,
sparkline fill, brightness/night mode, empty/max watchlists, maximum Wi-Fi
list, offline/reconnect, region resync, OTA, navigation stress and a 60-minute
memory/stack soak. Use the Phase 16.5 report template in
`docs/validation/pre-phase-17-release-readiness.md`.
