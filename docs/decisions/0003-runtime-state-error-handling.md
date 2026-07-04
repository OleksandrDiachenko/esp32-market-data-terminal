# 0003: Runtime state model, error taxonomy, and REST resync policy

## Status

Accepted (2026-07-04)

## Context

Phase 8 needed a shared runtime state model for the watchlist's market
data, plus a documented answer to "when does a failure retry, and when
does it halt the app." Three sub-decisions came up while designing
`components/app_state`.

## Decision

### 1. Fatal vs recoverable errors: two categories, not a graded severity scale

This project already has two consistent error-handling categories in
practice, from Phase 2 onward - Phase 8 documents them rather than
inventing a third:

- **Fatal** (`main/esp32-market-data-terminal.c`): `startup_diagnostics()`,
  `app_lifecycle_start()`, `display_ui_start()`, and now `app_state_init()`
  log an error and `return` from `app_main()`, halting startup. These are
  all resource/hardware-integrity problems (flash/PSRAM diagnostics, NVS
  init, display bring-up, PSRAM allocation for klines buffers) where
  continuing would run the rest of the app on a broken foundation.
- **Recoverable / soft dependency** (`wifi_manager`, `time_sync`,
  `market_data_client`, and now `app_state_sync_task`): log a warning and
  keep going. Established in Phase 5 ("Wi-Fi is not required for the UI to
  be useful") and reused by every network-adjacent module since.

`app_state`'s own pieces split across both: `app_state_init()` only
touches NVS and PSRAM (no network), so a failure there is a genuine
resource problem, not "no network yet" - it's fatal, matching
`app_lifecycle_start()`/`display_ui_start()`. `app_state_sync_task_start()`
only fails if task creation itself fails (out of memory), and the task's
entire job is best-effort market data - that's soft, matching
Wi-Fi/time_sync.

Within the soft-dependency category, `market_data_err_t` values are
further split by `app_state_retry_is_recoverable()`
(`app_state_retry_policy.c`) into:
- **Recoverable** (retried with backoff): `NETWORK`, `TIMEOUT`,
  `HTTP_STATUS`, `RATE_LIMITED`, `NOT_SYNCED`, `PARSE`, `NO_MEM` - all
  either transient (network/rate-limit/clock-not-ready-yet) or not provably
  permanent (a parse failure could be a one-off truncated response, not a
  Binance schema change).
- **Unrecoverable** (not retried automatically): `INVALID_ARG`,
  `SYMBOL_NOT_FOUND` - retrying the identical request cannot change the
  outcome; only a watchlist edit (out of scope for this task) can.

### 2. Per-symbol state table, not a single-symbol snapshot

The device will show several watchlist symbols at once
(`SETTINGS_MAX_WATCHLIST` = 8), so `app_state` holds an array of per-symbol
entries (state, last error, retry count, last sync time, klines), each
independently advancing through `INIT -> SYNCED -> DEGRADED -> SYNCED` (or
`-> ERROR`, terminal until the watchlist changes). This also gives Phase
9's WebSocket consumer a natural place to land per-symbol live updates
without restructuring - it writes into the same table by symbol index.

Connectivity-gap tracking (see below) is deliberately *not* part of this
per-symbol table: losing Wi-Fi affects every symbol equally, so it is
tracked once, privately, inside `app_state_sync_task.c`.

### 3. REST is a bootstrap/resync mechanism, not continuous polling

`market_data_client_fetch_klines_24h_5m_batch()` (see
[0002](0002-market-data-client.md) for why REST at all) is called by
`app_state_sync_task` only when a symbol is:
- never synced (`APP_STATE_SYMBOL_INIT`), or
- `APP_STATE_SYMBOL_DEGRADED` and its backoff delay
  (`app_state_retry_backoff_delay_ms()`, same exponential-with-cap formula
  as `wifi_policy_backoff_delay_ms()`) has elapsed, or
- every symbol at once, when a Wi-Fi reconnect follows a disconnected span
  of at least one candle interval (5 minutes -
  `app_state_retry_needs_resync()`), since the existing klines history may
  have a hole in it that a plain retry wouldn't fill.

A symbol already `SYNCED` is left alone otherwise. Once Phase 9's
WebSocket kline stream is live, it becomes the source of continuous
updates between these REST sync points - REST never runs on a fixed
polling timer.

## Consequences

- `app_state_sync_task` is the sole consumer of
  `wifi_manager_get_event_queue()` - it's a point-to-point FreeRTOS queue,
  not a broadcast, so no other module can also read it without stealing
  events from this task. A future consumer (e.g. a Wi-Fi status icon) needs
  its own fan-out, not a second reader on the same queue.
- The sync task keeps its own PSRAM scratch klines buffers (one per
  watchlist slot) separate from `app_state`'s internal storage, so that
  every write to the shared state still goes through the mutex-guarded
  `app_state_record_success()`/`_error()` instead of a second writer
  touching that memory directly. This roughly doubles the PSRAM committed
  to klines (up to ~360KB total for a full 8-symbol watchlist) - accepted
  given the board's 32MB PSRAM budget.
- An empty watchlist (`settings_store`'s default) is not an error -
  `app_state_symbol_count()` is 0 and the sync task idles, logging once,
  until symbols are configured (no watchlist-editing UI exists yet).

## Alternatives considered

- **Single global app state (one symbol) with multi-symbol as a later
  migration:** rejected - the device already needs to show several symbols
  at once, and retrofitting a single-symbol struct into a table later would
  touch every call site twice instead of once.
- **Gap-detection state stored per-symbol:** considered, since it was
  initially described that way while planning this phase, but a Wi-Fi
  disconnect is a single shared event, not a per-symbol one - tracking it
  once in the sync task avoids redundant, always-identical fields on every
  table entry.
- **Fixed-interval REST polling (e.g. every 5 minutes regardless of
  state):** rejected once the WebSocket design (Phase 9) was set - polling
  a REST endpoint on a timer duplicates what the live stream already
  provides and adds unnecessary request volume for a device with a
  power/bandwidth budget.
