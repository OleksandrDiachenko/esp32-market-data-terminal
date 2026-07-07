# 0008: Fix stale REST sync count and add live WS resubscribe on watchlist edits

## Status

Accepted (2026-07-07)

## Context

A user-reported bug: adding a symbol on the Add Symbol screen left it stuck
in "Loading" on the dashboard forever. Removing an already-tracked symbol
afterward would make the new one show data, but it still never
live-updated.

Root cause turned out to be two issues, only one of which
[0007](0007-watchlist-management.md) actually documented:

1. **An undocumented bug**: `app_state_sync_task.c`'s `sync_task_fn` read
   `app_state_symbol_count()` once at task start into a local `count` and
   never re-read it - its scratch-buffer array and its due-fetch loop
   (`collect_due_indices`) were both bounded by that frozen value. A symbol
   added later via `app_state_add_symbol()` is appended past that boundary,
   so it was never REST-synced and stayed in `APP_STATE_SYMBOL_INIT`
   ("Loading...") indefinitely. 0007 §3 claimed "REST sync picks up any
   symbol in the table on its next sweep regardless of when it was added" -
   that claim was false against the shipped code. Removing a different
   symbol appeared to "fix" the new one only because `app_state_remove_symbol`
   shifts later indices down, which could coincidentally move the new
   symbol inside the sync task's stale window - a side effect, not a real
   resync.
2. **A documented, deliberate gap** (0007 §3, "Alternatives considered"):
   the live WebSocket stream's combined `@kline_1s` subscription URL is
   built once at boot and was never revisited on watchlist edits, deferred
   pending evidence it mattered in practice. It now does.

## Decision

### 1. `app_state_sync_task.c` re-reads the watchlist every cycle

`sync_task_fn` now pre-allocates PSRAM scratch buffers for all
`APP_STATE_MAX_SYMBOLS` slots up front (cheap: ~25KB x 10 = 250KB against
this board's 32MB PSRAM budget, the same math 0007 already used for the
8->10 bump) and calls `app_state_symbol_count()` fresh on every loop
iteration instead of caching it once. A symbol added at runtime is now
REST-synced on the very next 2-second sweep, matching what 0007 originally
(incorrectly) claimed already happened.

### 2. Live SUBSCRIBE/UNSUBSCRIBE control frames on watchlist edits

Rather than tearing down and rebuilding the whole `esp_websocket_client`
connection (0007's rejected alternative, whose cost was clean shutdown +
resubscribe + re-verifying against the SDIO heap-corruption bug's
conditions), this adds Binance's control-frame protocol on the
**already-open** connection:

- `app_state.h`/`.c` gains a `app_state_watchlist_event_t` (ADDED/REMOVED +
  symbol) pushed onto a new `app_state_get_watchlist_event_queue()` by
  `app_state_add_symbol()`/`app_state_remove_symbol()` after each mutation
  commits - the same single-owner/single-consumer queue idiom already used
  for `wifi_manager_get_event_queue()` and
  `market_data_ws_client_get_update_queue()`.
- `market_data_ws_client` gains `market_data_ws_client_subscribe()` /
  `_unsubscribe()`, each building a
  `{"method":"SUBSCRIBE"/"UNSUBSCRIBE","params":["<symbol>@kline_1s"],"id":N}`
  frame (pure builder in `market_data_ws_control.c`, host-tested like the
  existing URL builder) and sending it via
  `esp_websocket_client_send_text()` on the existing connection. No
  reconnect, no new `esp_websocket_client_start()` call.
- `app_state_ws_task` now blocks on a FreeRTOS queue set covering both the
  existing kline-update queue and the new watchlist-event queue, so it can
  react to either without polling. `find_index_by_symbol` looks up against
  the live `app_state_symbol_count()` instead of a boot-time snapshot, so
  ticks for a just-added symbol resolve to the right slot immediately.

This does **not** reopen [0004](0004-websocket-streaming.md)'s decision:
that ADR is about the *initial* connection's design (URL-is-the-
subscription, no control frame needed at connect time) and stands
unchanged. This decision only adds control frames for *runtime edits* to a
connection that's already open and stable - the SDIO heap-corruption bug
0004/0007 both reference was specifically about racing
`esp_websocket_client_start()` against Wi-Fi STA bring-up at boot, which
sending a text frame on a long-established socket does not do.

## Consequences

- A newly-added watchlist symbol now leaves "Loading" within one sync
  cycle (~2s) and starts receiving live per-second ticks without a reboot.
  Removing a symbol unsubscribes it immediately; any in-flight tick for it
  that arrives anyway is still safely dropped by
  `app_state_apply_kline_update()`'s existing bounds check.
- 0007 §3's claim about REST sync behavior was inaccurate; this document
  supersedes that specific claim (0007's other content - the 8->10 bump,
  the add/remove API shape - is unaffected).
- No coverage was added for `app_state.c`, `app_state_sync_task.c`, or
  `app_state_ws_task.c` themselves (impure: PSRAM/mutex/FreeRTOS task
  entrypoints, consistent with this codebase's existing host-test
  boundary) - the new pure control-message builder is covered instead.
- The lack of hardware coverage here did let a real bug through:
  `ws_task_fn()`'s `xQueueAddToSet()` calls had their return values
  discarded, and `market_data_ws_client_start()` began receiving ticks
  before the queue-set registration ran - FreeRTOS requires a queue to be
  empty when joining a set, so a tick arriving in that window could
  silently and permanently orphan the update queue. Found and fixed during
  the `docs/debugging/watchlist-add-freeze.md` investigation by splitting
  `market_data_ws_client_start()` into `_create()`/`_connect()` so the
  queue set is joined while still provably empty, before connecting.
