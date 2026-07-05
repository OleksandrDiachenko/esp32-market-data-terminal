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

Pinned dependency versions (`components/wifi_manager/idf_component.yml`):

```yaml
espressif/esp_wifi_remote: "1.3.0"
espressif/esp_hosted: "==2.9.3"
```

These match ESP-IDF 6.0.1 and the C6 slave firmware version already on this
board family. Upgrading either requires re-validating on real hardware
before merging — see the `docs/validation/wifi-*.md` reports for what
"working" looked like at this pin.

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

## Addendum (2026-07-05): attempted esp_hosted 2.9.3 → 2.12.9 upgrade, reverted

Investigated upgrading `esp_hosted` (2.9.3 → 2.12.9), `esp_wifi_remote`
(1.3.0 → resolver-picked 1.6.1), and ESP-IDF (6.0.1 → 6.0.2), motivated by
an esp_hosted 2.11.0 changelog entry ("remove double freeing of buffer if
`rx()` fails") that looked like it could fix the pre-existing vendored
SDIO double-free documented in Phase 9's validation
(`docs/validation/websocket-streaming-hardware-test.md`).

Built and flashed cleanly (host tests all pass, `idf.py build` clean), but
hardware boot loops every time with:

```
transport: SDIO mode: slave: streaming, host: packet
transport: SDIO mode mismatch: slave is in streaming mode, but host is in packet mode. Aborting.
assert failed: process_init_event transport_drv.c:881 (0)
```

**Root cause:** esp_hosted's host code added a hard SDIO-mode compatibility
gate somewhere between 2.9.3 and 2.12.9. Our host Kconfig has always
selected packet mode (`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE=y`,
unchanged by this upgrade attempt) for the documented heap-fragmentation
reason above. The ESP32-C6's already-flashed slave firmware (2.12.8,
unchanged - this project never reflashes the C6) operates in streaming
mode. The old host (2.9.3) only logged this as a non-fatal version-mismatch
warning (`Host [2.9.0] < Co-proc [2.12.0]`, see
`docs/validation/wifi-hosted-link-bring-up.md`); the new host aborts.

**Reverted**: pins are back to `esp_hosted==2.9.3` / `esp_wifi_remote==1.3.0`
/ `idf: ">=6.0.1"`, confirmed working again on hardware (Wi-Fi connects,
WebSocket connects, REST sync succeeds) - see
`docs/validation/esp-hosted-2.12.9-upgrade-attempt.md`.

**Real paths forward, neither attempted here:**
1. Switch the host to `CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE`
   to match the slave - but this is the exact mode this ADR turned off
   because it triggered heap-fragmentation asserts during long HTTPS
   transfers, which is this project's core workload (REST/WS over TLS).
   Would need real hardware soak-testing under load before trusting it.
2. Reflash the ESP32-C6 with newer (packet-mode-capable) slave firmware -
   out of scope: this project doesn't own a C6-flashing process, and the
   C6 is shared with the sibling `siswood-air-quality` project on the same
   physical board.

Not resolving either path now; staying on 2.9.3 until one is deliberately
scoped as its own piece of work.
