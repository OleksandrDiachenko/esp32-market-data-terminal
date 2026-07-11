# 0009: Regional server auto-selection

## Status

Accepted (2026-07-11)

## Context

Binance.com's Terms of Use prohibit access from the United States - a U.S.
person must use Binance.US (`api.binance.us` / `stream.binance.us`)
instead. `api_region_settings_t` and its `select_base_url()`/
`select_base_ws_url()` consumers (Phase 7/9) already exist for exactly this
split, but nothing has ever called `settings_store_save_api_region()` - the
region has been permanently `SETTINGS_API_REGION_INTL` on every real build,
with no UI and no automatic logic behind it. Phase 13 closes that gap by
deriving the region from the time zone the user already picked in
`Settings > Time > Time zones` (`locale_settings_t.tz_label`), rather than
requiring a U.S. user to discover and flip a manual toggle themselves.

## Decision

### 1. An explicit `tz_label` allow-list, not a zone or prefix rule

`display_ui.c`'s `TZ_ZONE_AMERICA` bucket already mixes U.S. cities with
Canada, Mexico, and Central/South America, and Honolulu is filed under
`TZ_ZONE_PACIFIC`, not `TZ_ZONE_AMERICA` - so neither the zone id nor an
`America/*` string-prefix rule can distinguish "U.S. person" from "Western
Hemisphere person." The mapping is instead a fixed list of exact
`tz_label` strings that already exist (or are added by this ADR) in
`main/display_ui.c`'s `s_timezones[]` table: `America/Anchorage`,
`America/Los Angeles`, `America/Denver`, `America/Phoenix`,
`America/Chicago`, `America/Dallas`, `America/New York`,
`Pacific/Honolulu`, `America/San Juan` (this table's existing stand-in
label for Puerto Rico), plus four new labels from decision 2:
`Pacific/Guam`, `America/St Thomas` (U.S. Virgin Islands),
`Pacific/Pago Pago` (American Samoa), `Pacific/Saipan` (Northern Mariana
Islands).

### 2. The tz table's U.S.-territory gap is closed as part of this phase

Before this ADR, `s_timezones[]` had no entry at all for Guam, the U.S.
Virgin Islands, American Samoa, or the Northern Mariana Islands - a user
physically in any of those territories couldn't even select their real
city, let alone get auto-routed to the right Binance host. Rather than
carry that as a documented limitation, this phase adds all four:

| Territory | Label | POSIX TZ | Notes |
|---|---|---|---|
| Guam | `Pacific/Guam` | `ChST-10` | no DST |
| U.S. Virgin Islands | `America/St Thomas` | reuses `TZ_P_AST_NO_DST` (`AST4`) | same no-DST rule as the existing Puerto Rico entry |
| American Samoa | `Pacific/Pago Pago` | `SST11` | no DST |
| Northern Mariana Islands | `Pacific/Saipan` | reuses Guam's `ChST-10` | same zone as Guam |

Each is one new `tz_city_id_t` + `s_tz_city_names[]` entry and one new
`TZROW(...)` line, following the exact pattern every other row in the
table already uses - no new code path, just data.

### 3. A persisted `region_source` flag (auto/manual), not silent overwrite

A user may deliberately be a non-U.S. person on a U.S.-zone clock, or vice
versa, so an automatic region change must not permanently win over a
manual choice made afterward. `api_region_settings_t` gains a
`region_source` field (`settings_api_region_source_t`:
`SETTINGS_API_REGION_SOURCE_AUTO = 0` / `_MANUAL = 1`, default `AUTO`),
following the same seal/validate/range-check pattern the existing `region`
field already uses. This bumps `SETTINGS_API_REGION_VERSION` 1 -> 2 -
same category of consequence as 0007's `SETTINGS_MAX_WATCHLIST` bump: any
already-persisted v1 blob resets to default on next boot, acceptable since
nothing has ever written this field in practice.

### 4. A new pure `settings_api_region_map` module, not logic inside `display_ui.c`

`settings_api_region_from_tz_label(const char *tz_label)` (new
`components/settings_store/src/settings_api_region_map.c` +
`include/settings_api_region_map.h`) does the allow-list lookup from
decision 1 and returns `SETTINGS_API_REGION_US` on a match,
`SETTINGS_API_REGION_INTL` otherwise (including empty/never-set). Kept out
of `display_ui.c` (LVGL-dependent, not host-testable) specifically so the
mapping itself - the thing with actual regulatory consequences - gets host
tests: every U.S. state/territory label, plus negatives for Canada,
Mexico, and Central/South America labels that share the same
`TZ_ZONE_AMERICA` bucket, proving the allow-list (not a zone rule) is what
actually excludes them.

### 5. Auto-apply on time-zone change, plus a manual override screen

`time_zone_city_row_click_cb()` (where `tz_label` is set and
`settings_store_save_locale()` is called) now also loads
`api_region_settings_t`, and - only when `region_source == AUTO` -
recomputes the region via `settings_api_region_from_tz_label()` and saves
it if it changed. A new `SETTINGS_VIEW_REGION` screen (`Settings > Time >
Region`, next to the existing "Time zones" row) offers three checkable
rows - Automatic / International / United States - using the same
fixed-checkmark toggle-row pattern the Time format screen already uses.
Choosing Automatic sets `region_source = AUTO` and immediately re-derives
from the current `tz_label`; choosing either explicit option sets
`region_source = MANUAL` and pins that region regardless of future
time-zone edits.

### 6. A region switch reuses Phase 8/9's existing resync/reconnect machinery

- **REST**: `app_state_sync_task.c`'s `s_force_resync_all` already forces
  a full watchlist re-fetch (used today only after a long Wi-Fi
  disconnect) but had no external setter. A new
  `app_state_sync_task_force_resync()` is called from the region-change
  handler.
- **WebSocket**: the stream's host is chosen once per (re)connect by
  `select_base_ws_url()`, but nothing previously forced a reconnect
  outside of an actual network drop. `app_state_watchlist_event_kind_t`
  gains a third kind, `APP_STATE_REGION_CHANGED` (no symbol payload),
  pushed onto the existing watchlist-event queue that
  `app_state_ws_task.c`'s `ws_task_fn()` already blocks on via its queue
  set. On that event, `ws_task_fn()` calls `market_data_ws_client_stop()`
  and re-runs its existing create -> join-to-set -> connect boot sequence,
  so `select_base_ws_url()` is re-read against the new region using the
  same connect ordering Phase 9 already requires to avoid the SDIO
  heap-corruption bug.

### 7. Symbol-unavailable-in-region reuses the existing error taxonomy

No new state or error code is introduced. A symbol not listed on the new
region's host already surfaces `MARKET_DATA_ERR_SYMBOL_NOT_FOUND` from the
forced resync's REST batch fetch, which `app_state_retry_is_recoverable()`
(0003) already classifies as unrecoverable -> `APP_STATE_SYMBOL_ERROR`,
which `update_row()` already renders as `"Unavailable"`. This satisfies
"no silent gap" without new code.

## Consequences

- `settings_store` host tests covering `api_region_settings_t` need
  updating for the new `region_source` field and its default/roundtrip/
  out-of-range behavior; a new `settings_api_region_map` host-test suite is
  added alongside them.
- Any device already in the field with a persisted `api_region_settings_t`
  blob resets to `SETTINGS_API_REGION_INTL` / `AUTO` on first boot after
  this update (version bump), same as 0007's watchlist-limit bump.
- A region switch now costs one full REST resync (all symbols refetched)
  and one WebSocket reconnect - acceptable, since this only happens on a
  deliberate time-zone change or manual region choice, not on a timer.
- The tz-zone table now has entries for all five populated U.S.
  territories relevant to Binance.US eligibility (Puerto Rico, Guam, USVI,
  American Samoa, Northern Mariana Islands).

## Addendum (2026-07-11): a 3-strike budget for decision 7, and a klines-path bug fix

Decision 7 as originally written assumed the REST batch klines fetch already
surfaced `MARKET_DATA_ERR_SYMBOL_NOT_FOUND` for a Binance `-1121 Invalid
symbol` response. In practice it did not: `contains_binance_error_code()`
(added for `fetch_symbol_status`/`fetch_ticker_24hr`) was never wired into
`market_data_client_fetch_klines()` or the batch variant, so a `-1121` body
fell through to the generic `status_to_generic_error()` path and came back as
`MARKET_DATA_ERR_HTTP_STATUS` - classified *recoverable* by
`app_state_retry_is_recoverable()`. The practical effect, seen on hardware
after a region switch: a delisted-or-region-unsupported symbol sat in
`APP_STATE_SYMBOL_DEGRADED` ("Resyncing...") forever, retried every 60s
indefinitely instead of ever reaching `APP_STATE_SYMBOL_ERROR`.

This addendum closes that gap and adds a bounded retry budget rather than
transitioning straight to `ERROR` on the first `-1121`:

- Both klines fetch paths now run the same `-1121` detection the other two
  endpoints already had.
- A new per-symbol `invalid_symbol_count` (alongside `retry_attempt` in
  `symbol_slot_t`/`app_state_symbol_meta_t`) tracks *consecutive*
  `MARKET_DATA_ERR_SYMBOL_NOT_FOUND` responses. A new pure policy function,
  `app_state_retry_invalid_symbol_is_recoverable(prior_strikes)` in
  `app_state_retry_policy.c`, keeps the symbol `DEGRADED` for the first
  `APP_STATE_MAX_INVALID_SYMBOL_ATTEMPTS - 1` (= 2) strikes and reports
  unrecoverable on the 3rd, at which point `app_state_sync_task.c` passes
  `recoverable=false` into `app_state_record_error()` as before, landing in
  `APP_STATE_SYMBOL_ERROR`. Three strikes rather than one guards against a
  single transient/misleading 400 (e.g. a slow exchangeInfo cache on
  Binance's side) prematurely burying a symbol that would have synced fine
  on the next attempt.
- `update_row()` now distinguishes this terminal case from other
  unrecoverable errors: `last_error == MARKET_DATA_ERR_SYMBOL_NOT_FOUND`
  renders "Unsupported" in `COLOR_MUTED` (permanently-dead framing) instead
  of the orange "Unavailable" used for every other `ERROR` cause, since this
  one is expected to persist for the rest of the session by design rather
  than clearing on its own.
- `apply_region_change()` now calls a new `app_state_reset_symbols_for_region_change()`
  whenever the resolved server actually changes (`region_new != region_old`
  - an AUTO/MANUAL source flip that resolves to the same server does not
  trigger it), zeroing every symbol's `invalid_symbol_count`,
  `retry_attempt`, `state` (-> `INIT`), and `kline_count` (klines buffers
  are left allocated, just treated as empty). A symbol ruled "Unsupported"
  on the old server gets a clean slate on the new one, and stale
  old-host chart data is cleared to "Loading..." rather than lingering
  through the resync.

## Alternatives considered

- **`America/*` prefix rule**: rejected - the existing tz table already
  files Canada/Mexico/Central/South America under the same `TZ_ZONE_AMERICA`
  bucket and Hawaii under `TZ_ZONE_PACIFIC`, so a prefix or zone-id rule
  would both over- and under-match U.S. persons.
- **Leave the territory gap undocumented/out of scope**: considered, since
  those four territories were already unselectable before this phase (a
  pre-existing limitation, not one introduced here) - rejected because the
  roadmap's allow-list explicitly names all five U.S. territories, and
  leaving four unreachable would make Phase 13's own allow-list incomplete
  by construction.
- **A single `region` field with no source flag, always overwritten from
  `tz_label`**: rejected - would silently discard a deliberate manual
  choice (e.g. a non-U.S. person who wants Binance.US pairs) every time the
  user touches the time-zone screen for an unrelated reason (e.g. DST
  city change).
