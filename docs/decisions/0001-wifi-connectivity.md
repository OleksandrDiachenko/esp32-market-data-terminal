# 0001: Wi-Fi connectivity approach

## Status

Accepted (2026-07-03)

## Context

Phase 5 of the roadmap needs network connectivity. The board's SoC,
ESP32-P4, has no native Wi-Fi or Bluetooth radio — see
`docs/hardware/jc4880p443c.md`. Wireless goes through the on-board
ESP32-C6 co-processor over an internal SDIO link.

## Decision

Use **ESP-Hosted** (SDIO transport) with **`espressif/esp_wifi_remote`** as
the client shim, so application code calls the normal `esp_wifi_*` /
`esp_netif` APIs and the remoting to the C6 is transparent. This is not a
new investigation: it is the same approach already proven on this exact
board model by a sibling project (`siswood-air-quality`), whose C6 has
already been flashed with ESP-Hosted slave firmware.

Pinned dependency versions (`components/wifi_manager/idf_component.yml`),
as of the 2026-07-05 addendum below:

```yaml
espressif/esp_wifi_remote: ">=1.3.1"  # resolver picks 1.6.1 against IDF 6.0.2
espressif/esp_hosted: "==2.12.9"
```

Originally pinned to `esp_hosted==2.9.3` / `esp_wifi_remote 1.3.0` / ESP-IDF
6.0.1, matching the C6 slave firmware version already on this board family
at the time. See the addendum for why and how this moved to 2.12.9, and
`docs/validation/wifi-*.md` for what "working" looked like at each pin.
Upgrading either requires re-validating on real hardware before merging.

### SDIO configuration (`sdkconfig.defaults`)

Slot 1, 4-bit bus, 40 MHz clock. Pins are board-specific
(`JC4880P443C_I_W` / `JC-ESP32P4-M3` module): CMD=GPIO19, CLK=GPIO18,
D0-D3=GPIO14-17, C6 reset=GPIO54. The C6 is hard-reset on every P4 boot
(`CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y`) so the link starts
from a known state.

RX streaming mode is deliberately **off**
(`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE=y`, streaming unset): on
this board family it has triggered ESP-Hosted internal DMA heap
fragmentation asserts during long HTTPS transfers. This matters directly
for Phase 6 (market data over TLS), so the setting is carried over now
rather than discovered later under load.

### Credential storage

Wi-Fi credentials are stored in a dedicated, **encrypted** NVS partition
(`wifi_cfg`, keys in `nvs_keys`) rather than the default `nvs` partition —
see the new `partitions.csv` and `CONFIG_NVS_ENCRYPTION=y`.

**Known limitation:** NVS encryption without flash encryption / secure boot
means the XTS keys stored in the `nvs_keys` partition are themselves
readable from an unprotected flash chip. This raises the bar above
plaintext storage (a casual flash dump doesn't reveal passwords) but is
not a defense against a determined attacker with physical access. This is
an explicit, accepted trade-off for a portfolio device, not a claim of
strong security — flash encryption/secure boot would be a separate,
larger decision.

## Consequences

- A new `components/wifi_manager` component owns all Wi-Fi state; no other
  component calls `esp_wifi_*` directly.
- The partition table changes from ESP-IDF's default to a custom
  `partitions.csv`. **Any board already flashed before this change needs a
  one-time `idf.py erase-flash`** before reflashing, since NVS partition
  offsets move.
- Wi-Fi bring-up failure (SDIO link, C6 not responding, memory pressure)
  must not prevent the display/UI from starting — `wifi_manager_start()`
  failure is logged and treated as non-fatal in `app_main`.
- Internal DMA-capable RAM is shared between the display (`board_jc4880p443c`,
  LVGL/MIPI-DSI buffers) and the ESP-Hosted SDIO mempool. The manager checks
  the largest free internal DMA block before calling `esp_wifi_init()` and
  fails gracefully (logged, non-fatal) rather than crashing if there isn't
  enough room — see `docs/validation/wifi-hosted-link-bring-up.md` for the
  actual numbers observed on this board.

## Alternatives considered

- **UART instead of SDIO for the P4↔C6 link:** lower throughput, no reason
  to prefer it here since SDIO is already proven on this exact board.
- **Plaintext NVS for credentials:** simpler, but stores Wi-Fi passwords in
  the clear on flash; rejected given this is a device users will actually
  configure with real home network credentials.

## Addendum (2026-07-05): esp_hosted 2.9.3 → 2.12.9, resolved

A first attempt at this bump (host pin only, C6 untouched) boot-looped:
newer host code hard-checks an SDIO packet/streaming mode match against
whatever the C6 is running, and the C6 (2.12.8, inherited from a sibling
project, never reflashed by this project) didn't match. Reverted; full
write-up of that attempt lives in git history
(`chore/esp-hosted-2.12.9-upgrade` branch/PR).

The real fix was [0005](0005-esp-hosted-slave-ota.md): a host-driven
mechanism to push new co-processor firmware over the existing SDIO
transport (no UART), so host and co-processor versions can be bumped
together instead of drifting apart. Using it:

1. Built the co-processor firmware at 2.12.9 (`tools/esp_hosted_slave/`,
   ESP-IDF v5.5.4 - see 0005 for why), explicitly forced into **packet
   mode** (`CONFIG_ESP_SDIO_STREAMING_MODE=n`,
   `tools/esp_hosted_slave/sdkconfig.packet_mode`) to match this project's
   host-side config - the slave project's own default is streaming mode,
   which is what caused the mismatch in the first place.
2. Pushed it via the *old* (2.9.3) host, which doesn't hard-check mode
   compatibility - confirmed the co-processor's actual firmware changed to
   2.12.9.
3. Only then bumped the host pin to 2.12.9 (+ ESP-IDF 6.0.1 → 6.0.2 in every
   `idf_component.yml`, + CI's `esp_idf_version` in
   `.github/workflows/build.yml`) and reflashed.

Result on hardware: `transport: SDIO mode: slave: packet, host: packet` -
clean match, no abort, no `Version mismatch` warning (the one present on
every boot log since Phase 5). Wi-Fi/WebSocket/REST all unaffected. See
`docs/validation/esp-hosted-2.12.9-slave-packet-mode-hardware-test.md`.
