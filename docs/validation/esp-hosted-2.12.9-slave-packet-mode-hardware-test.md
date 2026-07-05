# Hardware validation: esp_hosted 2.12.9 bump, resolved via packet-mode slave OTA

## Environment

- Date: 2026-07-05
- Board: JC4880P443C_I_W (ESP32-P4 + ESP32-C6 module `JC-ESP32P4-M3`)
- Host: esp32p4, ESP-IDF v6.0.2, `esp_hosted==2.12.9`, `esp_wifi_remote`
  resolver-picked `1.6.1`
- Co-processor firmware built: esp32c6, ESP-IDF v5.5.4, `esp_hosted`
  2.12.9's vendored `slave/` project, **packet mode forced**
  (`tools/esp_hosted_slave/sdkconfig.packet_mode`,
  `CONFIG_ESP_SDIO_STREAMING_MODE=n`)
- Port: /dev/cu.usbmodem101
- Starting state: host 2.9.3 + co-processor 2.9.3 (both from
  `docs/validation/esp-hosted-slave-ota-hardware-test.md`)

## Method

Three-step sequence, deliberately in this order so the host never has to
tolerate a real mode mismatch on its own:

```sh
# 1. Bump idf_component.yml pins (esp_hosted 2.9.3 -> 2.12.9, esp_wifi_remote
#    ">=1.3.1", idf ">=6.0.2" in every component manifest), fetch new sources
source ~/esp/esp-idf-v6.0.2/export.sh
idf.py reconfigure   # vendors esp_hosted 2.12.9's slave/ under managed_components

# 2. Build the co-processor firmware at 2.12.9, forced to packet mode, and
#    push it via the CURRENTLY-RUNNING 2.9.3 host (still on the board,
#    tolerant of any co-processor version/mode - it only logs a warning)
source ~/esp/esp-idf-v5.5.4/export.sh
tools/esp_hosted_slave/build_slave_fw.sh /tmp/slave-build-2129
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem101 write-flash --force \
  0xD40000 /tmp/slave-build-2129/network_adapter.bin
# (host detects the mismatch and pushes/activates automatically on next boot)

# 3. Only once the co-processor is confirmed on 2.12.9: build and flash the
#    host itself
source ~/esp/esp-idf-v6.0.2/export.sh
idf.py build && idf.py -p /dev/cu.usbmodem101 flash
```

## Step 2 result: co-processor moved to 2.12.9 under the old (tolerant) host

```text
W (3334) transport: Version mismatch: Host [2.9.0] < Co-proc [2.12.0] ==> Upgrade host to avoid compatibility issues
I (4574) wifi_manager: ESP32-C6 co-processor firmware: 2.12.9
I (5774) wifi_manager_slave_ota: Co-processor firmware: running=2.12.9 embedded=2.12.9
I (5774) wifi_manager_slave_ota: Co-processor firmware already up to date
```

The push/activate/restart cycle completed in the gap between the `esptool
write-flash` command's own auto-reset and this capture starting (same as
the first slave-OTA validation) - by the time of this capture the
co-processor was already confirmed running 2.12.9. The inverted mismatch
warning (`Host [2.9.0] < Co-proc [2.12.0]`) is expected and non-fatal on
this still-2.9.3 host, exactly as it was in the original (pre-upgrade)
state, just with the version numbers swapped.

## Step 3 result: matched host+co-processor, clean SDIO mode negotiation

```text
I (3287) transport: SDIO mode: slave: packet, host: packet
I (4567) wifi_manager: ESP32-C6 co-processor firmware: 2.12.9
I (4607) wifi_manager_slave_ota: Co-processor firmware: running=2.12.9 embedded=2.12.9
I (4607) wifi_manager_slave_ota: Co-processor firmware already up to date
I (4917) RPC_WRAP: ESP Event: Station mode: Connected
I (5937) esp_netif_handlers: sta ip: 192.168.100.238, mask: 255.255.255.0, gw: 192.168.100.1
I (7567) market_data_ws_client: WebSocket connected
I (8457) app_state_sync: Synced 'BTCUSDT': 288 candles
I (8457) app_state_sync: Synced 'ETHUSDT': 288 candles
```

This is the exact failure this whole investigation started from
(`docs/validation/esp-hosted-2.12.9-upgrade-attempt.md`:
`SDIO mode mismatch: slave is in streaming mode, but host is in packet
mode. Aborting.`, boot-looping every cycle) - now resolved: both sides
report `packet` mode, no abort, no assert.

A second, longer (90s) capture showed no crashes, no asserts, no
unexpected errors - Wi-Fi, WebSocket, and REST all functioning normally
throughout.

## Host tests

All 5 host-side suites (`wifi_manager`, `settings_store`,
`market_data_client`, `app_state`, `market_data_ws_client`) pass unchanged
- this work only touches dependency pins, CI config, and the co-processor
build tooling, not host-testable application logic.

## Result

**Passed.** `esp_hosted` 2.9.3 → 2.12.9 (host) is complete, along with a
matching, packet-mode co-processor firmware pushed via the mechanism from
ADR 0005 - no UART, no physical access to the board. CI's
`esp_idf_version` bumped to `v6.0.2` in `.github/workflows/build.yml` to
match the new `idf: ">=6.0.2"` constraint.

## Follow-up

- The co-processor is now running firmware built with `esp_hosted` 2.12.9
  in packet mode - a real, non-default configuration for that project.
  If a *future* co-processor firmware bump reverts to the vendored
  defaults (streaming mode) without noticing
  `tools/esp_hosted_slave/sdkconfig.packet_mode`, the same SDIO mode
  mismatch will resurface. Always build the co-processor firmware via
  `tools/esp_hosted_slave/build_slave_fw.sh`, not a bare `idf.py build`
  in `managed_components/espressif__esp_hosted/slave/`.
