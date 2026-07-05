# Hardware validation: host-driven ESP32-C6 slave OTA (ADR 0005)

## Environment

- Date: 2026-07-05
- Board: JC4880P443C_I_W (ESP32-P4 + ESP32-C6 module `JC-ESP32P4-M3`)
- Host target: esp32p4, ESP-IDF v6.0.1, `esp_hosted==2.9.3` (unchanged - see
  ADR 0001/0005)
- Slave firmware built: esp32c6, ESP-IDF v5.5.4 (see ADR 0005 for why
  v6.0.1 doesn't work for this build), `esp_hosted` 2.9.3's vendored
  `slave/` project, default config (`CONFIG_ESP_SDIO_STREAMING_MODE=y`,
  the project's own default)
- Port: /dev/cu.usbmodem101
- Starting co-processor state: firmware 2.12.8 (from prior work in this
  session/sibling project), streaming mode - mismatched against the
  pinned host's packet-mode config, logging `Version mismatch: Host
  [2.9.0] < Co-proc [2.12.0]` on every boot

## Method

```sh
# 1. Add slave_fw partition (partitions.csv), new wifi_manager_slave_ota
#    component wired into wifi_manager_start()
idf.py build && idf.py -p /dev/cu.usbmodem101 flash

# 2. Build the co-processor image separately (ESP-IDF v5.5.4, esp32c6)
#    and write it into the host's own slave_fw data partition
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem101 write-flash \
  --force 0xD40000 network_adapter.bin
```

## Attempt 1: image-size bug, activation rejected

First boot after flashing detected the mismatch and pushed the image:

```text
I (4546) wifi_manager: ESP32-C6 co-processor firmware: 2.12.8
W (5546) wifi_manager_slave_ota: Could not read running co-processor version; assuming update is needed
W (5546) wifi_manager_slave_ota: Co-processor firmware differs from embedded 2.9.3 - pushing update over SDIO
I (15426) wifi_manager_slave_ota: Pushed 1187761 bytes of co-processor firmware
E (15446) wifi_manager_slave_ota: esp_hosted_slave_ota_activate failed: ESP_ERR_OTA_VALIDATE_FAILED - co-processor still holds the new image but hasn't switched to it
```

Two things noted and fixed here (both in `wifi_manager_slave_ota.c`):

1. The very first `esp_hosted_get_coprocessor_fwversion()` call (inside the
   new version-compare path) failed once, immediately after the existing
   `log_coprocessor_version()` had just succeeded a moment earlier - a
   transient RPC-channel-busy condition, not a real error. Added a 5x/200ms
   retry loop matching the pattern `log_coprocessor_version()` already
   uses.
2. `1187761` bytes pushed vs the actual built image being `1187776` bytes
   (`network_adapter.bin`, `0x121fc0`) - a 15-byte shortfall. Root cause:
   image-size reconstruction padded the segment data to a 16-byte boundary
   *before* adding the 1-byte checksum, when the checksum byte is actually
   part of what's padded. Truncated image, correctly rejected by the
   co-processor's own validation - no corruption, just a refused activate.
   Fixed the padding order in `parse_embedded_image()`. The co-processor
   was untouched (still 2.12.8) since activation never completed.

## Attempt 2: fixed, full success (unattended)

Rebuilt and reflashed only the host app (the `slave_fw` partition already
held a complete, correct image - only the host's *size calculation* was
wrong, not what was on flash). The push→activate→restart cycle completed
during the gap between the flash command's own auto-reset and this
session's next command - by the time a fresh capture was taken, the cycle
had already finished successfully:

```text
I (4535) wifi_manager: ESP32-C6 co-processor firmware: 2.9.3
I (5735) wifi_manager_slave_ota: Co-processor firmware: running=2.9.3 embedded=2.9.3
I (5735) wifi_manager_slave_ota: Co-processor firmware already up to date
```

- No `Version mismatch` warning anywhere in this boot (previously present
  on every single boot log throughout this project, back to Phase 5).
- No `SDIO mode mismatch` (the failure mode from the separate 2.12.9
  upgrade attempt, ADR 0001 addendum) - not applicable here since the host
  side was never bumped, only the co-processor.
- Wi-Fi, WebSocket, and REST all unaffected in the same capture:

```text
I (10475) RPC_WRAP: ESP Event: Station mode: Connected
I (11495) esp_netif_handlers: sta ip: 192.168.100.238, mask: 255.255.255.0, gw: 192.168.100.1
...
I (17825) market_data_ws_client: WebSocket connected
I (19085) app_state_sync: Synced 'BTCUSDT': 288 candles
I (19085) app_state_sync: Synced 'ETHUSDT': 288 candles
```

- No asserts/crashes/reboots across the full 90s capture.

## Result

**Passed** (after fixing the two issues found above). The co-processor's
*actual* firmware version changed from 2.12.8 to 2.9.3 as a direct result
of this mechanism, over SDIO, with no UART/physical access - confirming
the end-to-end push → activate → co-processor reboot → host resync cycle
works on this hardware. Subsequent boots correctly skip the OTA
(version-match fast path).

## Follow-up

- This proves the mechanism at a same-family version bump (2.9.3 host,
  now-matching 2.9.3 co-processor). The actual motivating case - bumping
  both host and co-processor to 2.12.9 together via this mechanism - is
  deliberately deferred to a follow-up (see ADR 0005 and the 2026-07-05
  addendum in ADR 0001): the co-processor's default build is streaming
  mode, which would need to be reconciled with the host's packet-mode
  config (or the host's) before that specific version combination is
  attempted again.
