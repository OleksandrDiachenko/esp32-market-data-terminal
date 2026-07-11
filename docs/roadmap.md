# Project Roadmap

## Goal

Build an ESP-IDF based ESP32-P4 market data terminal as a professional embedded firmware portfolio project.

## Current status

- Phases 0-11 done: board bring-up, Wi-Fi, settings/time sync, REST +
  WebSocket market data, runtime state, OTA updates, and the dashboard UI
  (watchlist, navigation shell, Settings) are all hardware-validated
- Phase 12 done: host-side tests were already comprehensive going in;
  added CI static analysis (cppcheck blocking, clang-format report-only)
  and a documented host-testable-module audit
- Phase 13 (regional server auto-selection) not started
- Phase 14 (branding, licensing & Binance data-usage compliance) not started
- Phase 15 (portfolio polish, renumbered from 13) not started

## Phases

### Phase 0: Project bootstrap
Status: Done

Acceptance criteria:
- [x] Git repository initialized
- [x] Basic ESP-IDF project builds
- [x] README/docs skeleton exists

### Phase 1: Board bring-up
Status: Done

Acceptance criteria:
- [x] ESP32-P4 target configured
- [x] Flash/PSRAM baseline configured
- [x] Startup diagnostics pass on hardware
- [x] Hardware test notes documented

### Phase 2: Application lifecycle
Status: Done

Acceptance criteria:
- [x] `app_main()` stays small
- [x] Application lifecycle module exists
- [x] Init/start failures return `esp_err_t`
- [x] Lifecycle logs are visible in monitor

### Phase 3: Minimal CI
Status: Done

Acceptance criteria:
- [x] GitHub Actions workflow added
- [x] Workflow runs `idf.py build` on push/PR to `main`
- [x] Build status is visible on pull requests

### Phase 4: Display/UI skeleton
Status: Done

Acceptance criteria:
- [x] Display board layer is isolated
- [x] Backlight/display init is documented
- [x] Minimal UI renders on screen
- [x] Display failure path is logged
- [x] Touch controller is initialized and registered as an LVGL input device
- [x] Touch failure path is logged

### Phase 5: Network connectivity
Status: Done

Acceptance criteria:
- [x] Connectivity approach for ESP32-P4 + ESP32-C6 is documented
- [x] Wi-Fi/hosted dependency decision is recorded
- [x] Network init has timeout/error handling
- [x] Wi-Fi credentials are stored encrypted, not in the repo
- [x] Autoconnect validated on real hardware

### Phase 6: Device settings & time sync
Status: Done

Acceptance criteria:
- [x] Settings persistence contract (display/symbols/locale) defined in `settings_store`
- [x] Settings stored unencrypted (default NVS partition) — no secrets involved
- [x] Host-side tests cover codec seal/validate/corruption paths
- [x] SNTP time sync starts after Wi-Fi connects and sets system time (UTC)
- [x] Time sync failure/timeout is logged and does not block the rest of the app (soft dependency, same category as Wi-Fi)
- [x] `locale_settings_t`'s `posix_tz` is applied (`setenv`/`tzset`) for local-time display, kept separate from the UTC time source used for TLS validation
- [x] SNTP sync validated on real hardware (JC4880P443C_I_W: connected to saved profile, got IP, `time_sync: System time synced via SNTP` logged ~0.5s later)

### Phase 7: Market data client
Status: Done

Note: depends on Phase 6's time sync — TLS certificate validation fails on
an unsynced clock (default epoch), independent of any Wi-Fi connectivity.
`market_data_client_fetch_symbol_status()`/`_fetch_klines()` check
`time_sync_is_synced()` before opening a connection.

Scope: REST only (no WebSocket — see
`docs/decisions/0002-market-data-client.md`), Binance `exchangeInfo` +
`klines` endpoints, fetch/parse/validate only — no UI/watchlist wiring, no
periodic polling (Phase 8).

Acceptance criteria:
- [x] Public market data endpoint selected — Binance public REST API,
      region-selectable base URL (`api_region_settings_t` in
      `settings_store`)
- [x] HTTP/TLS timeout handling exists — `market_data_http.c`
      (`timeout_ms`, `MARKET_DATA_ERR_TIMEOUT`), TLS via the existing
      certificate bundle (`esp_crt_bundle_attach`)
- [x] JSON parser handles success/error paths — custom incremental
      streaming parser (`market_data_json_scanner.c` +
      `market_data_klines_parser.c` + `market_data_symbol_parser.c`),
      covered by host tests including malformed/truncated input and
      arbitrary chunk-boundary splits
- [x] No API keys or secrets are required — public endpoints only, no auth
      headers
- [x] Validated on real hardware (JC4880P443C_I_W, via a temporary smoke-test
      hook since Phase 7 has no `app_main` wiring of its own): connected to
      saved Wi-Fi profile, time synced, `fetch_symbol_status("BTCUSDT")` ->
      `is_trading=1 has_spot=1`, `fetch_klines_24h_5m("BTCUSDT")` ->
      `count=288` with real Binance prices — see
      `docs/validation/market-data-client-hardware-test.md`

### Phase 8: Runtime state + error handling
Status: Done

Scope: per-symbol runtime state for the watchlist (`components/app_state`),
REST bootstrap/resync orchestration on top of Phase 7's
`market_data_client_fetch_klines_24h_5m_batch()`, and the fatal-vs-
recoverable error taxonomy this project already follows in practice - see
`docs/decisions/0003-runtime-state-error-handling.md`.

Acceptance criteria:
- [x] `app_state` holds per-symbol connection/data state with thread-safe
      accessors (`app_state_get_symbol_meta()`/`_get_symbol_klines()`),
      backed by PSRAM klines buffers sized for the full watchlist
      (`SETTINGS_MAX_WATCHLIST` = 8)
- [x] `market_data_err_t` is classified recoverable/unrecoverable
      (`app_state_retry_is_recoverable()`)
- [x] Backoff retry on recoverable errors - host-tested
      (`app_state_retry_backoff_delay_ms()`,
      `test_app_state_retry_policy.c`)
- [x] Gap-detection resync (disconnect >= one 5m candle interval forces a
      full watchlist re-fetch on reconnect) - host-tested
      (`app_state_retry_needs_resync()`)
- [x] `app_state_sync_task` fetches due watchlist symbols via the Phase 7
      batch API and updates per-symbol state; wired into `app_main()`
      after Wi-Fi/time_sync start
- [x] Fatal vs recoverable error taxonomy documented
      (`docs/decisions/0003-runtime-state-error-handling.md`)
- [x] Validated on real hardware (JC4880P443C_I_W): bootstrap of the whole
      watchlist, a short disconnect (no forced resync), a long disconnect
      (forced resync), and recoverable-error retry with exponential
      backoff - all four logged, see
      `docs/validation/app-state-runtime-hardware-test.md`. Also surfaced
      and fixed a pre-existing Wi-Fi empty-SSID profile corruption bug in
      `wifi_manager` (Phase 5) along the way - see the same report.

### Phase 9: Real-time WebSocket streaming
Status: Done

Note: depends on Phase 8's runtime state model (live updates need
somewhere in application state to land) and Phase 7's
`api_region_settings_t` (WS endpoint is region-selected the same way as
the REST base URL): `wss://stream.binance.com:9443` international /
`wss://stream.binance.us:9443` US (port 443 also valid for both).

Scope: `{symbol}@kline_1s` combined-stream only (one connection for the
whole watchlist) - no depth/trade/ticker streams. Each 1s update is merged
into the same 5m candle series REST already polls
(`market_data_client_fetch_klines_24h_5m`) via standard exchange
candle-update rules (update the in-progress 5m candle, or roll over to a
new one) - see
[0004: WebSocket kline streaming](decisions/0004-websocket-streaming.md)
for the full design and why `@kline_1s` rather than `@kline_5m` directly.

Acceptance criteria:
- [x] WebSocket endpoint selected and region-aware (reuses `api_region_settings_t`)
- [x] Connection lifecycle handled: TLS WSS connect, reconnect on drop
      (esp_websocket_client's own fixed-delay reconnect, see ADR 0004 §6),
      clean shutdown - no crash/hang if the stream is unavailable (same
      soft-dependency treatment as Wi-Fi/time_sync). Subscription is via
      the combined-stream URL itself, not a runtime SUBSCRIBE frame (ADR
      0004 §4).
- [x] Kline stream event JSON parsed (event type/time, symbol, kline
      payload: open/high/low/close/volume, `x` close flag)
- [x] Live candle updates land in Phase 8's runtime state model
- [x] No API keys or secrets are required
- [x] Validated on real hardware: live candle ticks between REST syncs,
      volume/trade-count advance only once per closed second, graceful
      behavior with Wi-Fi down (see
      `docs/validation/websocket-streaming-hardware-test.md` - also caught
      and fixed a hardware-only heap-corruption bug from starting the WS
      client before Wi-Fi connected)

### Phase 10: OTA firmware update via GitHub Releases
Status: Done

Scope: `esp_https_ota` + `esp_crt_bundle_attach` (same TLS pattern as
`market_data_client`'s REST calls), firmware artifact sourced from this
repo's public GitHub Releases via the `/releases/latest` API. Trigger is
manual (from the Phase 11 Settings screen) plus a periodic background
check that only sets an "update available" indicator - no silent
auto-flash.

Delivery plan (same PR-per-slice approach as Phase 5's Wi-Fi bring-up and
Phase 11's dashboard UI - this phase-level checklist is the combined
Definition of Done, not one PR):
1. ADR + partition table - `ota_0`/`ota_1` layout (replacing `factory`),
   firmware versioning scheme, `esp_http_client_config_t.buffer_size`
   sizing for GitHub's redirect `Location` header
2. OTA client: release check + image download - `/releases/latest` fetch,
   version-tag parsing/comparison, `esp_https_ota` flash (same
   TLS/`esp_crt_bundle_attach` pattern as `market_data_client`)
3. Background periodic check + manual trigger - "update available" flag
   in `app_state`, CLI/log-driven "check now"/"update now" until Phase
   11's Settings screen exists
4. Rollback safety - `esp_ota_mark_app_valid_cancel_rollback()` wired into
   startup, anti-rollback boot-loop protection
5. Hardware validation - a real OTA flash from a GitHub release plus a
   deliberate bad-image rollback test, closing out the acceptance
   criteria below

Acceptance criteria:
- [x] ADR documenting the approach: `ota_0`/`ota_1` partition table (the
      existing ~9 MB gap in `partitions.csv` between `factory`'s end and
      `nvs_keys` was reserved for this), firmware versioning scheme
      (GitHub release tag == firmware version), `esp_http_client_config_t`
      `buffer_size` sized for GitHub's long redirect `Location` header -
      see `docs/decisions/0006-ota-firmware-update.md`
- [x] Background periodic check of `/releases/latest` sets an
      "update available" flag in app state, surfaced once Phase 11's
      Settings screen exists - `app_state_ota_task` (every 6h, soft
      dependency like the REST sync task), flag in
      `app_state_get_ota_info()`/`_set_ota_info()`
- [x] Manual "check now" / "update now" trigger (CLI/log-driven until
      Phase 11's UI exists, then wired into Settings) - `ota_check`/
      `ota_update` console commands (`main/ota_console.c`); hardware
      validation found this REPL's physical console (UART0) isn't
      reachable over this board's currently-connected USB port (only its
      secondary USB-Serial-JTAG output mirror is) - code is correct and
      build-verified, real interactive use needs the other USB-C port or
      a UART-TTL adapter - see
      `docs/validation/ota-firmware-update-hardware-test.md`
- [x] Rollback on a bad image validated
      (`esp_ota_mark_app_valid_cancel_rollback` / anti-rollback boot-loop
      protection) - deliberately broken `0.10.2` test release crashed
      before confirming valid; bootloader auto-reverted to `0.10.1`,
      reproduced twice - see
      `docs/validation/ota-firmware-update-hardware-test.md`
- [x] No new secrets required (public repo, public release assets)
- [x] Validated on real hardware: a real OTA flash from a GitHub release,
      plus a deliberate bad-image rollback test - `0.10.0` -> `0.10.1` via
      a published GitHub Release (JC4880P443C_I_W), NVS/Wi-Fi/co-processor
      firmware all intact after the `factory` -> `ota_0`/`ota_1` partition
      change with no `erase-flash`; also caught and fixed a background
      OTA-check retry bug (didn't retry for 6h after a transient
      NOT_SYNCED failure) - see
      `docs/validation/ota-firmware-update-hardware-test.md`

### Phase 11: Market data dashboard UI
Status: Done

Scope: connect `display_ui`/LVGL to `app_state` and `settings_store` - a
real watchlist screen instead of the current static label, plus a
settings screen. Two screens minimum, per the reference hardware layout
(320x480, 8 symbols, ~57px rows, 24px bottom bar) scaled up for this
board's 480x800 panel.

Delivery plan (same PR-per-slice approach as Phase 5's Wi-Fi bring-up -
this phase-level checklist is the combined Definition of Done, not one
PR):
1. Watchlist rendering - LVGL row objects wired to `app_state`,
   point-in-place updates, no Settings screen yet - **Done**
2. Navigation shell - bottom bar, Watchlist <-> Settings screen switching -
   **Done** (Settings screen is a placeholder until slices 3-5 add real content)
3. Settings: connectivity & locale - Wi-Fi, existing `locale_settings`
   exposed in UI (no new `settings_store` schema) - **Done** (Wi-Fi
   scan/connect/password/forget-network screens; Settings > Time with
   24h toggle, Date Format, Time Zones, POSIX TZ editor)
4. Settings: watchlist management - add/remove symbols,
   `SETTINGS_MAX_WATCHLIST` 8->10 bump (own ADR, PSRAM impact) - **Done**
   (Manage + Add symbol screens, drag-and-drop reordering, live
   resubscribe on edits - see
   [0007](decisions/0007-watchlist-management.md) and
   [0008](decisions/0008-watchlist-live-resubscribe.md))
5. Settings: Updates entry - wired to Phase 10's OTA trigger - **Done**
   (tinted status card - Not checked yet/Update available/Up to date -,
   current/latest version + last-checked time, one context-dependent
   action button - Check for updates/Checking.../Install update -, and a
   real download-progress bar via `esp_https_ota`'s streaming
   begin/perform/finish API; `main/ota_console.c`'s CLI trigger removed as
   its own header called it a stand-in for this screen)
6. Hardware validation - full watchlist + settings sweep on real
   hardware, closing out the acceptance criteria below - **Done** (see
   `docs/validation/dashboard-ui-hardware-test.md`)

Acceptance criteria:
- [x] Watchlist: 10 rows at ~76px + a ~40px bottom bar (480x800), each row
      a distinct LVGL object updated in place (price/change/sparkline from
      `app_state`) - no full-list redraw per tick (`s_rows[]` in
      `display_ui.c`; actual row height is 58px, not the original ~76px
      estimate - 10 rows plus the status bar still fit the 800px panel
      without scrolling)
- [x] `SETTINGS_MAX_WATCHLIST` raised from 8 to 10 in `settings_store`/
      `app_state` - touches the PSRAM klines buffers sized in Phase 8
      (+25% memory for the watchlist); document this change in an ADR
      alongside this phase - see
      [0007](decisions/0007-watchlist-management.md)
- [x] Bottom bar: navigation between Watchlist and Settings (date/time on
      the left, a Settings button on the right, matching the reference
      device)
- [x] Settings: watchlist symbols (add/remove, within the new limit of
      10) - done; Wi-Fi connection - done; existing `locale_settings` -
      done; an "Updates" entry wired to Phase 10's OTA check/trigger -
      done (slice 5 above)
- [x] Connection/error state (Wi-Fi down, resync in progress) is visible
      on screen, not just in logs - status bar shows
      Connected/Connecting/Reconnecting/Offline, per-row "Resyncing..."
      during a forced resync
- [x] Touch is used for real navigation (bottom bar taps, list scroll/select,
      drag-and-drop reordering on the watchlist manage screen)
- [x] Validated on real hardware with live data - see
      `docs/validation/dashboard-ui-hardware-test.md`

### Phase 12: Host-side tests + CI hardening
Status: Done

Scope: this phase turned out not to be greenfield - by the time it was
picked up, 7 components already had host tests (~25 test files, ~360
assertions) covering every parser and state machine in the codebase, wired
into CI's `host-tests` job. The real net-new work was CI hardening (static
analysis was completely absent) and a documented audit of which modules
are host-testable and why the rest aren't, so that claim is verifiable
rather than assumed - see `docs/testing.md`.

Delivery plan:
1. CI: cppcheck (blocking, scoped to the same pure-logic files host tests
   already compile) + clang-format (report-only, no `.clang-format` existed
   before so a full-tree reformat diff hasn't been reviewed) - **Done**
2. Docs: host-testable module audit in `docs/testing.md` + this Phase 12
   closeout - **Done**

Acceptance criteria:
- [x] Host-testable modules identified - see the "Host-tested (pure
      logic)" / "Not host-tested, and why" split in `docs/testing.md`;
      every non-test-covered file was checked individually for separable
      pure logic
- [x] Parser/state machine tests added - already true going into this
      phase (every parser and state machine in the codebase has host
      tests); no net-new test files were needed, so none were added just
      to pad the checklist
- [x] CI runs host-side unit tests on push/PR - already true (`host-tests`
      job in `.github/workflows/build.yml`), formally closed out here;
      this phase additionally added `cppcheck` (blocking) and
      `format-check` (non-blocking, report-only) jobs
- Explicitly deferred: automated secrets-scanning in CI (AGENTS.md
  mentions it; today it's a manual PR-checklist item only) - out of scope
  for this phase, candidate for a future `chore/*` slice

### Phase 13: Regional server auto-selection
Status: Planned

Scope: derive the Binance REST/WS host from the user's selected time zone
instead of relying on a manual region toggle - pick Binance.US
(`api.binance.us` / `stream.binance.us`,
`SETTINGS_API_REGION_US`) for any time zone that belongs to United States
territory, Binance.com (`SETTINGS_API_REGION_INTL`) for everyone else.
Regulatory driver: Binance.com is not available to U.S. persons, so a U.S.
user must be steered to Binance.US automatically rather than having to know
to flip a setting. Builds on the already-separate `api_region_settings_t`
domain ([settings_codec.h](../components/settings_store/include/settings_codec.h))
and the `tz_label` ("Zone/City") already stored in `locale_settings_t`.

Open design questions (to resolve in the ADR before coding):
- The definitive set of U.S. time zones: the 50 states + D.C. plus U.S.
  territories (Puerto Rico, Guam, U.S. Virgin Islands, American Samoa,
  Northern Mariana Islands). `America/*` alone is wrong (it also covers
  Canada / Mexico / Central & South America), so the mapping must be an
  explicit allow-list of `tz_label`s, maintained next to the existing
  time-zone city tables in `display_ui.c`.
- Auto vs. manual: auto-set the region when the time zone changes, but keep
  a manual override afterward (user may deliberately be a non-U.S. person on
  a U.S. clock, or vice-versa). Needs a "region source" flag
  (auto/manual) so a manual choice isn't silently re-overwritten on the next
  time-zone edit.
- Symbol-universe mismatch: Binance.US lists fewer pairs than Binance.com;
  a switch to US may leave watchlist symbols that don't exist there.
  Decide surfacing (per-symbol "unavailable in this region" state, reusing
  the existing per-symbol error state) - no silent data gaps.
- A region switch must force a full resync + WebSocket resubscribe (same
  path as the Phase 8/9 resync), since the underlying host changed.

Acceptance criteria:
- [ ] ADR documenting the U.S.-territory `tz_label` allow-list, the
      auto/manual region-source flag, and the resync-on-switch behavior
- [ ] Region is auto-selected from the selected time zone (U.S. zone ->
      `SETTINGS_API_REGION_US`, otherwise `SETTINGS_API_REGION_INTL`)
- [ ] Manual override is preserved across subsequent time-zone edits
      (region-source flag), host-tested
- [ ] Switching region forces a resync + WS resubscribe; no stale data from
      the previous host
- [ ] Symbols unavailable on the active region surface a visible per-symbol
      state, not a silent gap
- [ ] Host tests cover the tz_label -> region mapping (U.S. states,
      territories, and non-U.S. `America/*` negatives)
- [ ] Validated on real hardware: pick a U.S. zone -> data comes from
      Binance.US; pick a non-U.S. zone -> data comes from Binance.com

### Phase 14: Branding, licensing & data-usage compliance
Status: Planned

Scope: the legal / first-run / branding layer that a portfolio-facing build
needs before it is shown publicly - an OSI license for the code, a SISWOOD
boot screen, visible Binance data attribution, and a one-time
disclaimer shown on first boot after a firmware update. All four share the
same new "first-run / branding" surface (boot screen + an NVS
"acknowledged disclaimer version" flag), so they are one phase delivered as
slices, not four phases.

Note on scope boundary: the code license (Apache-2.0) is independent of
Binance's Terms of Use for the *data*. Binance's one hard restriction is
that you may not charge for or otherwise profit from (ads, referral fees)
market data pulled from their API - satisfied automatically by a free,
non-commercial open-source build. The attribution + disclaimer slices are
best-practice compliance and honesty to the user, not a hard technical
mandate from Binance.

Delivery plan (PR-per-slice, this checklist is the combined Definition of
Done):
1. LICENSE - add an Apache License 2.0 `LICENSE` file at repo root
   (user's request: "something like Apache, for free personal use"),
   add SPDX/`NOTICE` as needed, and note the Binance-data non-commercial
   caveat in the README so the code license isn't mistaken for a data
   license.
2. SISWOOD boot/splash screen - a screen shown at startup before the
   dashboard: centered "SISWOOD" wordmark, firmware version (from
   `esp_app_desc_t.version` / `version.txt`), an "Open Source" line, and a
   footer with the license ("Apache License 2.0") and "Market data by
   Binance". Follow the `/dashboard-design` tokens.
3. Binance data attribution - a persistent, non-intrusive "Market data by
   Binance" attribution reachable from the running UI (bottom-bar /
   Settings > About), plus an informational disclaimer entry (data may be
   delayed/inaccurate, not financial advice).
4. First-run-after-update disclaimer - a full-screen disclaimer shown once
   after each firmware update (or when the disclaimer text version bumps),
   gated by an "acknowledged disclaimer version" value in NVS; user must
   tap Accept to reach the dashboard. Reuses the Phase 6 settings/NVS
   pattern. Disclaimer text drafted below.

Acceptance criteria:
- [ ] `LICENSE` (Apache-2.0) at repo root; README clarifies code license vs.
      Binance data terms (non-commercial use of the data)
- [ ] Boot screen shows SISWOOD + firmware version + "Open Source" +
      license/attribution footer, styled per `/dashboard-design`
- [ ] "Market data by Binance" attribution + informational disclaimer
      reachable from the running UI (About/Settings)
- [ ] First-run-after-update disclaimer shown once per firmware/disclaimer
      version, acknowledgement persisted in NVS, gates entry to the
      dashboard
- [ ] Disclaimer / acknowledgement-version logic host-tested (shows on
      version change, stays hidden once acknowledged)
- [ ] Validated on real hardware: fresh boot shows splash + disclaimer;
      reboot without a version change skips the disclaimer; an OTA update
      re-shows it

Draft disclaimer text (slice 4, English - UI language):

> **Before you start**
>
> SISWOOD Market Terminal is a free, open-source project provided for
> informational and educational purposes only.
>
> - Market data is provided by Binance and may be delayed, incomplete, or
>   inaccurate.
> - Nothing shown here is financial, investment, or trading advice.
> - Do not rely on this device for trading decisions - always verify prices
>   on the official exchange.
> - The software is provided "as is", without warranty of any kind. Use at
>   your own risk.
>
> Tap **Accept** to continue.

Draft boot-screen footer (slice 2):

> SISWOOD - v{firmware_version} - Open Source
> Licensed under Apache License 2.0 - Market data by Binance

### Phase 15: Portfolio polish
Status: Planned

Acceptance criteria:
- [ ] README explains project value clearly
- [ ] Architecture docs are updated
- [ ] Screenshots/photos/log evidence are added
- [ ] Git history shows small reviewed changes