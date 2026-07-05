# 0007: Watchlist management - symbol limit bump and runtime add/remove

## Status

Accepted (2026-07-05)

## Context

Phase 11's reviewed dashboard design (slice 4) needs the Settings screen to
add/remove watchlist symbols without a reboot, and the roadmap calls for
raising `SETTINGS_MAX_WATCHLIST` from 8 to 10 alongside it. Both touch
`settings_store` (the persisted symbol list) and `app_state` (the runtime
per-symbol state table + PSRAM klines buffers built on top of it), so they
get one combined decision rather than two silent drive-by changes.

## Decision

### 1. `SETTINGS_MAX_WATCHLIST` 8 -> 10

A `#define` bump in `settings_codec.h`. `app_state`'s
`APP_STATE_MAX_SYMBOLS` is already defined as `SETTINGS_MAX_WATCHLIST`
([0003](0003-runtime-state-error-handling.md)), so its per-symbol state
array and `display_ui`'s row/scratch arrays (both sized off
`APP_STATE_MAX_SYMBOLS`) scale automatically - no separate constant to
keep in sync.

**Memory impact**: each watchlist slot's klines buffer is
`sizeof(market_data_kline_t) * MARKET_DATA_KLINES_V1_LIMIT` in PSRAM
(~25 KB per Phase 8's own sizing note). Going from 8 to 10 slots adds two
more such buffers - roughly +50 KB PSRAM, +25% of the watchlist's total
klines footprint. This board has 32 MB of PSRAM
(`docs/hardware/jc4880p443c.md`); the increase is noise against that
budget and doesn't change `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` or any
other PSRAM-adjacent Kconfig value.

### 2. `app_state` gets a runtime add/remove API

Until now `app_state`'s symbol table was load-once:
`app_state_init()` reads `settings_store_load_symbols()` and allocates
each slot's PSRAM klines buffer exactly once at boot - there was no path
to add or remove a slot afterward, because nothing needed one before
Settings UI existed. Slice 4 adds two functions to
`components/app_state/include/app_state.h`:

```c
esp_err_t app_state_add_symbol(const char *ticker);
esp_err_t app_state_remove_symbol(uint8_t index);
```

Both take the same lock as the existing accessors/writers, allocate/free
that one slot's PSRAM klines buffer on add/remove, and shift the array on
remove (the table has no gaps - every consumer already iterates
`0..app_state_symbol_count()-1`). `app_state_add_symbol()` fails with
`ESP_ERR_NO_MEM` if the watchlist is already at `APP_STATE_MAX_SYMBOLS` -
`display_ui` is expected to disable its own "Add symbol" entry point at
the limit rather than rely on this as the primary guard, but the function
enforces it regardless since it's cheap to and the caller shouldn't be
the only thing standing between two writers and an out-of-bounds slot.

`display_ui`'s Settings screens call these functions directly and persist
the same change to `settings_store_save_symbols()` - `app_state` doesn't
call back into `settings_store` itself, keeping the "who persists what"
boundary the same as everywhere else in this codebase (callers persist,
`app_state` is runtime-only).

### 3. The live WebSocket stream does *not* pick up an added symbol until reboot

[0004](0004-websocket-streaming.md) builds `app_state_ws_task`'s combined
`@kline_1s` stream URL once, from whatever the watchlist was when
`app_state_ws_task_start()` ran at boot, deliberately sequenced after
Wi-Fi connects to avoid the SDIO heap-corruption bug documented in
`docs/validation/websocket-streaming-hardware-test.md`. Tearing that
connection down and rebuilding the subscription URL every time a symbol
is added or removed is a real feature, not a two-line change, and isn't
needed to satisfy this slice's acceptance criteria (which only asks for
add/remove UI within the new limit, not live-stream resubscription).

A symbol added at runtime still gets real data immediately: REST sync
(`app_state_sync_task`) picks up any symbol in the table on its next
sweep regardless of when it was added. Only the *between-REST-syncs*
per-second tick updates are missing until the next reboot restarts the
WS task against the full, current watchlist. This is called out here
so it isn't mistaken for a bug later - live resubscription is deferred to
a follow-up if it turns out to matter in practice.

## Consequences

- `settings_store`/`app_state` host tests that assume `SETTINGS_MAX_WATCHLIST
  == 8` need updating to 10 alongside this change.
- A newly-added symbol shows live per-second ticks only after the next
  reboot; until then it updates on the same 5-minute REST cadence as
  everything else. Documented on the Add Symbol screen is out of scope for
  this slice - the gap is between reboots, not a data-correctness issue.
- Removing a symbol frees its PSRAM klines buffer immediately; the
  now-invalid WS subscription entry for it (if any) is simply ignored by
  `app_state_apply_kline_update()`'s existing bounds check until the next
  reboot rebuilds the subscription without it.

## Alternatives considered

- **Keep the limit at 8, add UI for add/remove only**: rejected - the
  roadmap already commits to 10 as part of this slice, and the reviewed
  design mockup was built around a 10-row watchlist.
- **Rebuild the WS subscription on every add/remove**: rejected for this
  slice - real engineering effort (clean shutdown of
  `esp_websocket_client`, resubscribe, re-verify against the SDIO
  heap-corruption bug's conditions) disproportionate to what this slice's
  acceptance criteria ask for. Revisit if live ticks on a
  just-added symbol turn out to matter to users in practice.
