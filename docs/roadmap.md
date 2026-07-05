# Project Roadmap

## Goal

Build an ESP-IDF based ESP32-P4 market data terminal as a professional embedded firmware portfolio project.

## Current status

- ESP-IDF project bootstrapped
- Documentation skeleton added
- ESP32-P4 startup diagnostics implemented
- Hardware startup diagnostics validated on JC4880P443C_I_W

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
Status: Planned

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
      `ota_update` console commands (`main/ota_console.c`) over the
      existing log UART
- [ ] Rollback on a bad image validated
      (`esp_ota_mark_app_valid_cancel_rollback` / anti-rollback boot-loop
      protection)
- [x] No new secrets required (public repo, public release assets)
- [ ] Validated on real hardware: a real OTA flash from a GitHub release,
      plus a deliberate bad-image rollback test

### Phase 11: Market data dashboard UI
Status: Planned

Scope: connect `display_ui`/LVGL to `app_state` and `settings_store` - a
real watchlist screen instead of the current static label, plus a
settings screen. Two screens minimum, per the reference hardware layout
(320x480, 8 symbols, ~57px rows, 24px bottom bar) scaled up for this
board's 480x800 panel.

Delivery plan (same PR-per-slice approach as Phase 5's Wi-Fi bring-up -
this phase-level checklist is the combined Definition of Done, not one
PR):
1. Watchlist rendering - LVGL row objects wired to `app_state`,
   point-in-place updates, no Settings screen yet
2. Navigation shell - bottom bar, Watchlist <-> Settings screen switching
3. Settings: connectivity & locale - Wi-Fi, existing `locale_settings`
   exposed in UI (no new `settings_store` schema)
4. Settings: watchlist management - add/remove symbols,
   `SETTINGS_MAX_WATCHLIST` 8->10 bump (own ADR, PSRAM impact)
5. Settings: Updates entry - wired to Phase 10's OTA trigger
6. Hardware validation - full watchlist + settings sweep on real
   hardware, closing out the acceptance criteria below

Acceptance criteria:
- [ ] Watchlist: 10 rows at ~76px + a ~40px bottom bar (480x800), each row
      a distinct LVGL object updated in place (price/change/sparkline from
      `app_state`) - no full-list redraw per tick
- [ ] `SETTINGS_MAX_WATCHLIST` raised from 8 to 10 in `settings_store`/
      `app_state` - touches the PSRAM klines buffers sized in Phase 8
      (+25% memory for the watchlist); document this change in an ADR
      alongside this phase
- [ ] Bottom bar: navigation between Watchlist and Settings (date/time on
      the left, a Settings button on the right, matching the reference
      device)
- [ ] Settings: watchlist symbols (add/remove, within the new limit of
      10), Wi-Fi connection, existing `locale_settings`, an "Updates"
      entry wired to Phase 10's OTA check/trigger
- [ ] Connection/error state (Wi-Fi down, resync in progress) is visible
      on screen, not just in logs
- [ ] Touch is used for real navigation (bottom bar taps, list scroll/select)
- [ ] Validated on real hardware with live data

### Phase 12: Host-side tests + CI hardening
Status: Planned

Acceptance criteria:
- [ ] Host-testable modules identified
- [ ] Parser/state machine tests added
- [ ] CI runs host-side unit tests on push/PR

### Phase 13: Portfolio polish
Status: Planned

Acceptance criteria:
- [ ] README explains project value clearly
- [ ] Architecture docs are updated
- [ ] Screenshots/photos/log evidence are added
- [ ] Git history shows small reviewed changes