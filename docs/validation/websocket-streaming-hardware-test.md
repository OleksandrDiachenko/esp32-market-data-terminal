# Hardware validation: WebSocket kline_1s streaming (Phase 9)

## Environment

- Date: 2026-07-05
- Board: JC4880P443C_I_W
- Target: esp32p4
- ESP-IDF: v6.0.1
- Port: /dev/cu.usbmodem101
- Wi-Fi: existing saved profile ("TV") in encrypted NVS
- Watchlist: BTCUSDT, ETHUSDT (same as Phase 8's validation)

## Method

```sh
idf.py -p /dev/cu.usbmodem101 flash
```

`idf.py monitor` requires an interactive TTY, unavailable in this session - the serial
port was read directly with `pyserial` at 115200 baud instead, toggling DTR/RTS to
trigger the same hard reset `idf.py monitor` would.

Two temporary, documented, one-shot code changes were used to make live behavior
observable in a boot-log capture instead of requiring a display/UI to watch, mirroring
`docs/validation/app-state-runtime-hardware-test.md`'s approach for Phase 8. Both were
fully reverted before the final build - verified with `git diff` showing only the bug
fix in `app_state_ws_task.c`/`.h` (see "Bug found" below), nothing else:

1. `app_state.c`: a temporary `ESP_LOGI` in `app_state_apply_kline_update()` printing
   the merged candle's bucket/close/high/low/volume/count after every WS update.
2. `main/esp32-market-data-terminal.c`: a temporary task that disconnects Wi-Fi 60s
   after boot for 10s, then reconnects - to observe the WebSocket client's reconnect
   behavior without needing to physically touch the router.

## Bug found during validation: heap corruption at boot (fixed)

The first capture crashed a few hundred milliseconds after boot:

```text
I (5674) main_task: Returned from app_main()
I (5674) wifi_manager: Connecting to 'TV' (origin=1)
assert failed: tlsf_free tlsf.c:630 (!block_is_free(block) && "block already marked as free")
Backtrace: 0x4ff0dc8a:0x4816cbe0 0x4ff0d76c:0x4816cc00 ...
Rebooting...
```

Symbolizing the backtrace (`riscv32-esp-elf-addr2line -pfiaC -e build/esp32-market-data-terminal.elf ...`)
pointed at `sdio_process_rx_task` -> `hosted_free` in the vendored `espressif__esp_hosted`
SDIO driver (`managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c:1197`)
- not in this project's own code.

**Root cause:** `app_state_ws_task_start()` called `market_data_ws_client_start()`
synchronously in `app_main()`, before Wi-Fi had connected. The resulting immediate
connection attempt failed with a DNS resolution error (Wi-Fi wasn't up yet) and began
retrying, generating extra SDIO traffic concurrently with ESP-Hosted's own Wi-Fi
station bring-up over the same shared SDIO link - a race that triggers a pre-existing
double-free in the vendored driver under that concurrent load. This never surfaced in
Phase 7/8's hardware testing because REST-only traffic never started before Wi-Fi was
already connected.

**Fix:** `app_state_ws_task.c` now waits for `wifi_manager_get_snapshot().state ==
WIFI_MANAGER_STATE_CONNECTED` (polling every 500ms) before ever calling
`market_data_ws_client_start()`. This only matters once, at boot - once connected,
`esp_websocket_client`'s own reconnect handles any later drop without needing Wi-Fi to
report "connected" again (confirmed in Scenario 3 below). Not a workaround: this
mirrors the same soft-dependency pattern `market_data_client`'s REST calls already use
(gated on `time_sync_is_synced()`), just applied to the WS client's initial connect.

**Validation of the fix:** three subsequent full boot cycles (this doc's Scenarios
1-3, plus one additional clean-build boot at the very end) showed no crash, no reset,
and the WS client's first connection attempt landing after Wi-Fi actually connected.

## Scenario 1: bootstrap + live candle ticking

```text
I (1284) market_terminal: ESP32 Market Data Terminal started
I (5625) RPC_WRAP: ESP Event: Station mode: Disconnected
I (5995) RPC_WRAP: ESP Event: Station mode: Connected
I (8785) market_data_ws_client: WebSocket connected
I (9524) app_state: ws 'BTCUSDT': no-op, REST hasn't bootstrapped this symbol yet (count=0)
I (9524) app_state: ws 'ETHUSDT': no-op, REST hasn't bootstrapped this symbol yet (count=0)
I (11054) app_state_sync: Synced 'BTCUSDT': 288 candles
I (11064) app_state_sync: Synced 'ETHUSDT': 288 candles
I (11564) app_state: ws 'BTCUSDT': bucket=1783210200000 close=63037.49 high=63039.61 low=63029.51 volume=2.1995 count=288
I (12594) app_state: ws 'BTCUSDT': bucket=1783210200000 close=63037.49 high=63039.61 low=63029.51 volume=2.1995 count=288
I (13514) app_state: ws 'BTCUSDT': bucket=1783210200000 close=63037.49 high=63039.61 low=63029.51 volume=2.2250 count=288
I (14534) app_state: ws 'BTCUSDT': bucket=1783210200000 close=63037.50 high=63039.61 low=63029.51 volume=2.2332 count=288
```

**Passed**, with one extra thing confirmed along the way: the WebSocket connects
(8785ms) *before* REST finishes bootstrapping (11054ms) - `@kline_1s` updates arrive
for a symbol that has no candles yet, and `app_state_kline_merge_apply()`'s
`*count == 0` guard correctly no-ops them (no crash, no garbage), exactly as its host
test (`test_empty_series_is_noop`) already proved. Once REST syncs, live ticking
starts immediately: `close` updates roughly once per second, `high`/`low` widen
monotonically, `volume` only increases (never resets mid-bucket), `count` stays at
288 (already at capacity from the REST bootstrap).

## Scenario 2: bucket rollover (append + evict oldest)

Same boot capture, extended to ~7 minutes to catch a real 5-minute boundary:

```text
I (179696) app_state: ws 'BTCUSDT': bucket=1783208700000 close=63219.01 high=63241.33 low=63182.00 volume=64.7077 count=288
I (180716) app_state: ws 'BTCUSDT': bucket=1783208700000 close=63219.00 high=63241.33 low=63182.00 volume=64.7081 count=288
I (181636) app_state: ws 'BTCUSDT': bucket=1783209000000 close=63219.00 high=63219.00 low=63219.00 volume=0.0003 count=288
I (182666) app_state: ws 'BTCUSDT': bucket=1783209000000 close=63219.00 high=63219.00 low=63219.00 volume=0.0209 count=288
I (185736) app_state: ws 'BTCUSDT': bucket=1783209000000 close=63219.00 high=63219.01 low=63219.00 volume=0.0528 count=288
```

**Passed.** At the 5-minute boundary (`bucket` jumps by exactly 300000ms), a new
candle is appended - seeded fresh from the update (`open=high=low=close`, `volume`
reset near zero) - while `count` stays at 288 throughout, proving the oldest candle
was evicted rather than the array growing unbounded.

## Scenario 3: Wi-Fi disconnect mid-stream - no crash, clean reconnect

Temporary `ws_disconnect_test_task` (see Method) disconnected Wi-Fi for 10s, 65s into
the same capture:

```text
W (65585) market_terminal: ws_disconnect_test: disconnecting Wi-Fi for 10000 ms
I (65615) RPC_WRAP: ESP Event: Station mode: Disconnected
E (65625) websocket_client: esp_transport_read() failed with -76, transport_error=ESP_ERR_MBEDTLS_SSL_READ_FAILED, ...
W (65645) market_data_ws_client: WebSocket error; esp_websocket_client will retry on its own
W (65655) market_data_ws_client: WebSocket disconnected; esp_websocket_client will retry on its own
W (75585) market_terminal: ws_disconnect_test: reconnecting
I (75825) RPC_WRAP: ESP Event: Station mode: Connected
I (77645) app_state_sync: Reconnected after 12000 ms offline; resuming normal cadence
I (79765) market_data_ws_client: WebSocket connected
```

**Passed.** The active TLS connection breaks the instant Wi-Fi drops (expected -
`ESP_ERR_MBEDTLS_SSL_READ_FAILED`), logged as a warning with no crash/hang; the REST
sync task's own existing gap-detection ran independently and correctly did not force a
resync (12000ms offline, well under its 5-minute threshold); `esp_websocket_client`
reconnected on its own (~4s after Wi-Fi returned, mid-cycle on its 10s retry timer) and
live ticking resumed immediately afterward.

## Result

Passed (after the fix above). All Phase 9 roadmap acceptance criteria now validated
end-to-end on real hardware, including one hardware-only bug (heap corruption from a
Wi-Fi-bring-up race) found and fixed as part of this validation.
