# 0005: Host-driven ESP32-C6 slave firmware updates

## Status

Accepted (2026-07-05)

## Context

[0001](0001-wifi-connectivity.md) pinned `esp_hosted==2.9.3` to match the
ESP32-C6 co-processor firmware already flashed on this board family by a
sibling project - this project has never had a way to update that firmware
itself. Its 2026-07-05 addendum documents why that became a real problem:
an attempted `esp_hosted` 2.9.3 → 2.12.9 host bump boot-looped, because the
newer host code hard-checks an SDIO packet/streaming mode match against
whatever the C6 happens to be running, and the C6 (2.12.8, untouched)
didn't match our host's config. The addendum identified two ways forward;
this ADR is the first of them made concrete: a repeatable way to update the
C6's firmware from the P4, so host and co-processor versions can be bumped
together instead of drifting apart.

`esp_hosted` ships exactly this as a supported, documented mechanism - the
host pushes a new co-processor image over the *existing* SDIO transport, no
UART/serial adapter or physical access to the board required
(`examples/host_performs_slave_ota/` in the vendored package, using
`esp_hosted_slave_ota_begin/write/end/activate()` from
`host/api/include/esp_hosted_ota.h`, all already present in our pinned
2.9.3).

## Decision

### Mechanism: partition-sourced push OTA, checked on every Wi-Fi bring-up

A new data partition, `slave_fw` (custom subtype `0x40`, 2 MB, offset
`0xD40000`), holds the co-processor's app image (`network_adapter.bin`)
built from the exact `esp_hosted` version this project pins. Appended after
the existing partitions with no offset changes to `nvs`/`wifi_cfg`/etc., so
this change does **not** require `erase-flash` on an already-flashed board.

`components/wifi_manager/src/wifi_manager_slave_ota.c`
(`wifi_manager_slave_ota_check_and_update()`), called from
`wifi_manager_start()` right after Wi-Fi comes up and the existing
`log_coprocessor_version()` runs:

1. Find the `slave_fw` partition; if missing or empty (all-`0xFF`), skip -
   soft dependency, same treatment as every other Wi-Fi bring-up step in
   this project.
2. Parse the embedded ESP-IDF app image's header for its version string
   (`esp_app_desc_t.version`) and true byte size.
3. Compare against `esp_hosted_get_coprocessor_fwversion()` (the C6's
   *actual* running version, read over the RPC channel, with retries - the
   first read right after transport bring-up is occasionally not ready
   yet).
4. If they match, log and stop.
5. If they differ: stream the partition's image to the C6 in 1500-byte
   chunks via `esp_hosted_slave_ota_begin/write/end()`, then
   `esp_hosted_slave_ota_activate()` (switches the C6's own OTA boot slot
   and reboots it), then `esp_restart()` the host after a short delay so
   both sides come up fresh and resync.

### Why check on every boot, not behind a manual trigger

The check is cheap when versions already match (one RPC round-trip) and
the whole point is to make host/co-processor version skew impossible to
forget about - the same reasoning as `log_coprocessor_version()`'s existing
unconditional log. A manual-only trigger would recreate exactly the
"nobody remembers to do this" failure mode that caused the 2.12.9 attempt
to blow up in the first place.

### Build recipe (documented, not automated in CI)

Building `slave/` (the vendored esp_hosted package's own co-processor
firmware project) for `esp32c6` **fails on ESP-IDF 6.0.1** - its
`sdio_slave_api.c` still includes `soc/sdio_slave_periph.h`, a path that
moved to `hal/sdio_slave_periph.h` under IDF 6.0's HAL component split. The
slave side of this esp_hosted release was evidently never updated for that
reorganization (only the host side is validated against IDF 6.0.x per
0001). Fix: build it with **ESP-IDF v5.5.4** instead - a completely
separate, independent build from the P4 host firmware, no version
coupling required between the two.

```sh
source ~/esp/esp-idf-v5.5.4/export.sh
cd managed_components/espressif__esp_hosted/slave
idf.py -B /path/to/slave-build \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6" \
  set-target esp32c6
idf.py -B /path/to/slave-build build
# -> /path/to/slave-build/network_adapter.bin

# Flash into the HOST's own slave_fw data partition (not the C6's flash -
# esptool needs --force since the image's chip ID is c6, not p4):
python -m esptool --chip esp32p4 -p <port> write-flash --force \
  0xD40000 /path/to/slave-build/network_adapter.bin
```

The result is version-pinned by the same `esp_hosted==2.9.3` this project
already requires in `components/wifi_manager/idf_component.yml` -
`managed_components/espressif__esp_hosted/slave/` always matches whatever
version the component manager resolved for the host side, since it's the
same vendored package.

### A real bug found and fixed along the way

The first hardware attempt pushed a real ~1.16 MB image but
`esp_hosted_slave_ota_activate()` failed with `ESP_ERR_OTA_VALIDATE_FAILED`.
Root cause: the image-size reconstruction (parsing the ESP-IDF app image
header + segments to know how many bytes to stream, since a data partition
doesn't otherwise know where the "real" image ends before the erased
`0xFF` padding starts) miscalculated by 15 bytes - it padded the segment
data to a 16-byte boundary *before* accounting for the 1-byte checksum,
when the checksum byte is actually part of what gets padded. This silently
truncated the pushed image by 15 bytes, and the co-processor correctly
refused to boot into a truncated image. Fixed in
`wifi_manager_slave_ota.c`'s `parse_embedded_image()`. Notably, this same
bug exists in the vendored `examples/host_performs_slave_ota/.../ota_partition.c`
reference this code was adapted from - not filed upstream, out of scope
for this project, but worth knowing if that example is ever copied again.

## Consequences

- Host and co-processor firmware versions can now be bumped together
  deliberately, closing the gap that caused the 2.12.9 attempt to fail.
- `wifi_manager_start()` now has a code path that calls `esp_restart()`
  and does not return, on the (expected to be rare) occasion the embedded
  slave image differs from what's running. Callers of `wifi_manager_start()`
  should not assume it always returns.
- Adds ~2 MB of otherwise-unused flash (`slave_fw` partition) - negligible
  against the 16 MB budget.
- The slave build process depends on a second ESP-IDF installation
  (v5.5.x) alongside the project's own v6.0.1/v6.0.2 - documented above,
  not wired into CI (this project's CI only builds the P4 host firmware).

## Alternatives considered

- **UART-based manual reflash of the C6**: works (used once historically),
  but requires physical access and doesn't scale to "reflash the co-processor
  on every meaningful esp_hosted bump" the way this project now needs.
- **HTTPS-sourced slave OTA** (fetch `network_adapter.bin` from a server at
  runtime, one of the three methods the vendored example supports): more
  moving parts (network dependency, hosting) for no benefit here - the
  slave image only needs to change when this project's own firmware is
  rebuilt and reflashed anyway, so embedding it in the same flash operation
  is simpler and has no extra runtime dependency.

## Validated on hardware

JC4880P443C_I_W: co-processor was on firmware 2.12.8 (mismatched with the
pinned host's 2.9.0 protocol, `Version mismatch: Host [2.9.0] < Co-proc
[2.12.0]` on every prior boot). After this mechanism ran once: pushed and
activated a freshly-built 2.9.3 co-processor image over SDIO (no UART), C6
rebooted, host resynced - `ESP32-C6 co-processor firmware: 2.9.3` on every
boot since, version-mismatch warning gone, Wi-Fi/WebSocket/REST all
unaffected (BTCUSDT/ETHUSDT synced, WebSocket connected, IP obtained). See
`docs/validation/esp-hosted-slave-ota-hardware-test.md`.
