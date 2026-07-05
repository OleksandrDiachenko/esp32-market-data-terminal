# Hardware validation: esp_hosted 2.9.3 → 2.12.9 upgrade attempt (reverted)

## Environment

- Date: 2026-07-05
- Board: JC4880P443C_I_W (ESP32-P4 + ESP32-C6 module `JC-ESP32P4-M3`)
- Target: esp32p4
- Port: /dev/cu.usbmodem101
- Attempted: ESP-IDF v6.0.2 (new `git worktree` checkout at
  `~/esp/esp-idf-v6.0.2`, toolchain installed separately from the existing
  v6.0.1 install so the known-good environment stayed intact), `esp_hosted`
  pinned `==2.12.9`, `esp_wifi_remote` relaxed to `>=1.3.1` (component
  manager resolved `1.6.1`)

## Motivation

`esp_hosted` 2.11.0's changelog ("remove double freeing of buffer if
`chan_arr[buf_handle->if_type]->rx()` fails") looked like a plausible fix
for the pre-existing vendored SDIO double-free found during Phase 9
hardware validation
(`docs/validation/websocket-streaming-hardware-test.md`). Also: the C6 is
already flashed with slave firmware 2.12.8, so the host was meaningfully
behind (`Host [2.9.0] < Co-proc [2.12.0]` warning at every boot).

## Method

```sh
# ESP-IDF v6.0.2, separate worktree from the pinned v6.0.1 install
cd ~/esp/esp-idf-v6.0.1 && git fetch origin tag v6.0.2
git worktree add ~/esp/esp-idf-v6.0.2 v6.0.2
cd ~/esp/esp-idf-v6.0.2 && git submodule update --init --recursive --depth 1
./install.sh esp32p4

# bumped components/wifi_manager/idf_component.yml:
#   idf: ">=6.0.2", esp_wifi_remote: ">=1.3.1", esp_hosted: "==2.12.9"
# (and idf min version in board_jc4880p443c/market_data_ws_client manifests)

source ~/esp/esp-idf-v6.0.2/export.sh
idf.py set-target esp32p4 && idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

## Result: build/flash clean, hardware boot loop

- All 5 host test suites still passed unchanged (277+ checks across
  `wifi_manager`, `settings_store`, `market_data_client`, `app_state`,
  `market_data_ws_client`) - expected, since none of this touches
  host-testable pure-C code.
- `idf.py build` succeeded with no warnings related to the version bump.
  Component manager resolved `esp_hosted==2.12.9`,
  `esp_wifi_remote==1.6.1` against ESP-IDF 6.0.2 without conflict.
- On hardware, every boot crashed within ~3.3 seconds of SDIO bring-up,
  reproduced across all 6 boot cycles in a 45s capture:

```text
I (3287) transport: Identified slave [esp32c6]
I (3287) transport: 	 * WLAN
I (3287) transport: 	   - HCI over SDIO
I (3287) transport: SDIO mode: slave: streaming, host: packet
E (3287) transport: SDIO mode mismatch: slave is in streaming mode, but host is in packet mode. Aborting.
assert failed: process_init_event transport_drv.c:881 (0)
Backtrace: 0x4ff0e1ac:0x4816fbe0 0x4ff0dc8e:0x4816fc00 0x4ff0b4a6:0x4816fd20 ...
Rebooting...
```

**Root cause:** see the ADR 0001 addendum. The new host code added a hard
SDIO packet/streaming mode compatibility check that the old host (2.9.3)
didn't have - it only logged the version mismatch as a warning and carried
on. Our host config has always requested packet mode
(`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE=y`); the already-flashed
C6 slave firmware (2.12.8, untouched by this attempt) runs in streaming
mode. New host + old slave = incompatible by the new gate's own logic.

## Revert and re-validation

Reverted `idf_component.yml` pins to `esp_hosted==2.9.3` /
`esp_wifi_remote==1.3.0` / `idf: ">=6.0.1"` (identical to what was on
`main`), rebuilt with the original ESP-IDF v6.0.1 environment, reflashed:

```text
I (3294) transport: Identified slave [esp32c6]
...
W (3334) transport: Version mismatch: Host [2.9.0] < Co-proc [2.12.0] ==> Upgrade host to avoid compatibility issues
I (3344) transport: Base transport is set-up, TRANSPORT_TX_ACTIVE
I (4524) wifi_manager: Wi-Fi station started over ESP-Hosted link
I (4544) wifi_manager: ESP32-C6 co-processor firmware: 2.12.8
I (5654) wifi_manager: Connecting to 'TV' (origin=1)
I (5974) RPC_WRAP: ESP Event: Station mode: Connected
I (6994) esp_netif_handlers: sta ip: 192.168.100.238, mask: 255.255.255.0, gw: 192.168.100.1
I (10014) market_data_ws_client: WebSocket connected
I (20994) app_state_sync: Synced 'BTCUSDT': 288 candles
```

**Passed** - full Wi-Fi/WebSocket/REST bring-up restored, no crash across
the 60s capture. One unrelated, pre-existing observation: `ETHUSDT`'s REST
sync failed with a recoverable error and retried with the expected
exponential backoff (4s → 8s → 16s) - this is Phase 8's documented retry
policy working as designed, not a regression from this investigation
(`BTCUSDT` on the same watchlist synced fine in the same run).

## Follow-up

- `git diff` against `main` is empty for all `idf_component.yml` files -
  no net dependency change landed from this investigation.
- `~/esp/esp-idf-v6.0.2` (git worktree, ~600MB) was left in place in case
  a follow-up attempt at one of the two paths in the ADR addendum wants it;
  remove with `git -C ~/esp/esp-idf-v6.0.1 worktree remove ~/esp/esp-idf-v6.0.2`
  if not needed.
- Local disk headroom was tight during this work (~9GB free of 460GB) -
  worth keeping in mind before installing more toolchain versions.
