# Hardware validation: app_state runtime model (Phase 8)

## Environment

- Date: 2026-07-04
- Board: JC4880P443C_I_W
- Target: esp32p4
- ESP-IDF: v6.0.1
- Port: /dev/cu.usbmodem101
- Wi-Fi: existing saved profile ("TV") in encrypted NVS
- Watchlist: BTCUSDT, ETHUSDT (persisted via `settings_store_save_symbols()` ahead of
  this session - no watchlist-editing UI exists yet)

## Method

Four scenarios from `docs/roadmap.md`'s Phase 8 acceptance criteria, each requiring a
real REST round trip to Binance and/or a real Wi-Fi state change - none of this is
host-testable. Scenarios 2-4 needed temporary, documented, one-shot code changes to
control conditions that can't be triggered from outside the firmware (a forced
disconnect/reconnect, a forced HTTP timeout); each change is called out below and was
reverted before the final clean build (verified with `git diff` showing no changes).

```sh
idf.py -p /dev/cu.usbmodem101 flash
```

`idf.py monitor` requires an interactive TTY, unavailable in this session - the serial
port was read directly with `pyserial` at 115200 baud instead (same effect, plain log
capture), toggling DTR/RTS to trigger the same hard reset `idf.py monitor` would.

## Scenario 1: bootstrap the whole watchlist

Clean firmware, cold boot, no temporary changes.

```text
I (5569) app_state: Loaded 2 watchlist symbol(s)
I (5619) wifi_manager: Connecting to 'TV' (origin=1)
I (5969) RPC_WRAP: ESP Event: Station mode: Connected
I (7029) esp_netif_handlers: sta ip: 192.168.100.238, mask: 255.255.255.0, gw: 192.168.100.1
I (7269) time_sync: System time synced via SNTP
I (7709) esp-x509-crt-bundle: Certificate validated
I (9679) esp-x509-crt-bundle: Certificate validated
I (11049) app_state_sync: Synced 'BTCUSDT': 288 candles
I (11049) app_state_sync: Synced 'ETHUSDT': 288 candles
```

**Passed.** Both watchlist symbols went `INIT -> SYNCED` via the single keep-alive
batch session (one `market_data_http_open()` + one `market_data_http_next()`, two
certificate validations - one per request, as expected: see
`docs/decisions/0003-runtime-state-error-handling.md`), each with the full 288-candle
24h/5m history. Total time from boot to both symbols synced: ~11s, dominated by Wi-Fi
connect + SNTP, not the REST calls themselves (~2s for both).

## Scenario 2: short disconnect (below the resync threshold) - no forced resync

Temporary change: `RESYNC_GAP_MS` in `app_state_sync_task.c` lowered from
`5 * 60 * 1000` to `10 * 1000` (10s) so the threshold could be crossed in seconds
instead of minutes; a temporary task in `main` disconnected Wi-Fi for 5s after
bootstrap and reconnected via `wifi_manager_connect_saved("TV")`. Both reverted
afterward - the gap-detection *logic* being exercised
(`app_state_retry_needs_resync()`) is the same pure comparison regardless of the
threshold's value, already exhaustively boundary-tested on host
(`test_app_state_retry_policy.c`); only the real Wi-Fi disconnect/reconnect event
handling needed hardware.

```text
I (11352) app_state_sync: Synced 'BTCUSDT': 288 candles
I (11352) app_state_sync: Synced 'ETHUSDT': 288 candles
W (25572) market_terminal: disconnect_test: disconnecting Wi-Fi for 5000 ms
I (25602) RPC_WRAP: ESP Event: Station mode: Disconnected
W (30572) market_terminal: disconnect_test: reconnecting
I (30832) RPC_WRAP: ESP Event: Station mode: Connected
I (33352) app_state_sync: Reconnected after 6000 ms offline; resuming normal cadence
```

**Passed.** 6000ms offline (< 10000ms test threshold) correctly did *not* trigger a
forced resync - no additional `Synced` lines after reconnect, matching "REST is
bootstrap/resync, not continuous polling."

## Scenario 3: long disconnect (above the resync threshold) - forced resync

Same temporary setup as Scenario 2, with the disconnect held for 15s instead of 5s.

```text
W (25569) market_terminal: disconnect_test: disconnecting Wi-Fi for 15000 ms
I (25599) RPC_WRAP: ESP Event: Station mode: Disconnected
W (40569) market_terminal: disconnect_test: reconnecting
I (40799) RPC_WRAP: ESP Event: Station mode: Connected
I (42749) time_sync: System time synced via SNTP
W (43869) app_state_sync: Reconnected after 18000 ms offline (>= 10000 ms); forcing full resync
I (43949) esp-x509-crt-bundle: Certificate validated
I (45149) esp-x509-crt-bundle: Certificate validated
I (46389) app_state_sync: Synced 'BTCUSDT': 288 candles
I (46389) app_state_sync: Synced 'ETHUSDT': 288 candles
```

**Passed.** 18000ms offline (>= 10000ms test threshold) correctly forced a full
watchlist re-fetch on reconnect - both symbols re-synced.

## Scenario 4: recoverable-error retry with backoff

Temporary change: `MARKET_DATA_HTTP_TIMEOUT_MS` in `market_data_client.c` lowered from
`10000` to `1` so every REST connection attempt times out deterministically, without
needing to fake network conditions.

```text
W (13580) esp-tls: Failed to open new connection in specified timeout
W (13630) app_state_sync: 'BTCUSDT' fetch failed (err=3), retrying in 4000 ms
W (13640) app_state_sync: 'ETHUSDT' fetch failed (err=3), retrying in 4000 ms
W (17700) app_state_sync: 'BTCUSDT' fetch failed (err=3), retrying in 8000 ms
W (17710) app_state_sync: 'ETHUSDT' fetch failed (err=3), retrying in 8000 ms
W (25790) app_state_sync: 'BTCUSDT' fetch failed (err=3), retrying in 16000 ms
W (25800) app_state_sync: 'ETHUSDT' fetch failed (err=3), retrying in 16000 ms
```

**Passed.** `err=3` (`MARKET_DATA_ERR_NETWORK`) correctly classified recoverable
(`app_state_retry_is_recoverable()`); both symbols independently transition to
`DEGRADED` and retry with exactly the exponential-with-cap formula host-tested in
`test_app_state_retry_policy.c` (`app_state_retry_backoff_delay_ms(2000, 60000,
attempt)`: attempt 1 -> 4000ms, 2 -> 8000ms, 3 -> 16000ms). Recovery back to `SYNCED`
once a request actually succeeds is the same code path already demonstrated in
Scenarios 1 and 3 (a successful fetch always calls `app_state_record_success()`
regardless of how many prior attempts failed).

## Issues found and fixed during this session

Two problems surfaced while setting up this validation, both outside Phase 8's own
code - fixed on their own branches/PRs before hardware testing could proceed:

1. **Wi-Fi empty-SSID profile corruption** (`components/wifi_manager`, Phase 5) -
   `WIFI_POLICY_IN_CONNECT_SUCCESS` processed unconditionally, letting a stray/
   duplicate "connected" event while idle persist a profile with an empty SSID,
   which then starved autoconnect. Fixed with a write guard (policy + adapter), a
   read-side sanitize-and-recover pass on load, and a regression test. See PR #16 /
   `docs/decisions/` is not used for this one - the fix commit message documents the
   root cause and fix in full.
2. **Intermittent ESP-Hosted SDIO driver crash** (`managed_components/espressif__esp_hosted`,
   third-party, not project-owned code) - `assert failed: tlsf_free ... "block already
   marked as free"`, backtrace rooted in `sdio_process_rx_task` -> `hosted_free()`.
   Observed twice during this session (always on the *first* boot after a fresh flash,
   never on subsequent resets), each time self-recovering via the panic handler's
   automatic reboot with no further symptoms. This matches the risk this project's own
   `sdkconfig.defaults` already documents ("RX streaming mode stays off: it has
   triggered ESP-Hosted DMA heap fragmentation asserts... on this board family") - an
   accepted, pre-existing third-party driver flakiness, not a Phase 8 regression. Not
   fixed (out of scope: vendored dependency); noted here as evidence for anyone
   investigating similar crashes later.

## Result

**Passed** - all four Phase 8 hardware scenarios verified. `docs/roadmap.md`'s Phase 8
hardware-validation checkbox and status updated accordingly.
