# 0010: Licensing, attribution, and first-run disclaimer

## Status

Accepted (2026-07-11)

## Context

Phase 14 is the legal / compliance layer a portfolio-facing build needs
before it is shown publicly: an OSI license for the code, visible Binance
data attribution, and a one-time disclaimer shown on first boot after a
firmware update. An initial pass also added a branded boot/splash screen
("SISWOOD" wordmark) as part of the same first-run surface; that was
removed after implementation - there is no app-branding screen in the
final design, only the disclaimer.

## Decision

### 1. Code license (Apache-2.0) is explicitly separate from Binance's data terms

`LICENSE` (Apache License 2.0, copyright Oleksandr Diachenko) and `NOTICE`
are added at repo root. Binance's Terms of Use forbid charging for or
profiting from (ads, referral fees) market data pulled from their API -
independent of, and not satisfied merely by, the code's license. A free,
non-commercial open-source build satisfies that restriction automatically.
The README's "License & data usage" section and the Settings > About
screen both state the two licenses separately, so the Apache-2.0 code
license is never mistaken for a grant to the Binance data itself.

### 2. No app-branding/boot-splash screen

A full-screen wordmark splash was built, verified on hardware, then
reverted wholesale (see git history: "Add SISWOOD boot splash screen" /
"Remove SISWOOD boot splash and branding"). The firmware boots directly to
the dashboard (or the disclaimer, if gated - decision 4). Binance
attribution and license info live in Settings > About instead of a splash
screen, reachable at any time rather than shown once and gone.

### 3. Attribution lives in Settings > About, not a persistent on-screen banner

`build_about_screen()` (main/display_ui.c) shows the app name, firmware
version, "Open Source - Apache License 2.0", "Market data by Binance", and
the full informational disclaimer paragraph. The Settings list row's own
description text ("Market data by Binance") doubles as a passive,
always-visible attribution without needing to enter the screen - chosen
over a persistent status-bar banner, which would compete with the
dashboard's own live price data for the same limited vertical space.

### 4. First-run disclaimer gated by firmware version, not a separate text-version counter

`disclaimer_settings_t` (settings_store, sealed-blob NVS domain like
display/symbols/locale/api_region) persists the firmware version string
the user last accepted the disclaimer for. `settings_disclaimer_should_show(acked, running)`
is a pure decision helper: true when never acknowledged or acknowledged
for a version other than the one currently running. This means:

- Every OTA update re-shows the disclaimer (the version string always
  changes on update).
- A plain reboot never re-shows it (the version string doesn't change).
- A disclaimer *text* change can only ship inside a firmware update
  anyway, so gating on firmware version alone covers text changes too -
  no separate "disclaimer text version" counter was needed.

The alternative (a dedicated disclaimer-text version, bumped only when the
copy changes) was rejected as unnecessary complexity: the two counters
would always move together in practice, since there is no mechanism to
change disclaimer text without shipping a new firmware version.

### 5. Disclaimer built as its own `lv_screen`, not an `lv_layer_top()` overlay

The dev-only "screenshot" console command
(`main/dev_screenshot_console.c`) snapshots via
`lv_snapshot_take_to_draw_buf(lv_screen_active(), ...)`, which only walks
the active screen's own object tree - it does not include `lv_layer_top()`,
even though LVGL's real rendering composites the top layer over the active
screen correctly. An overlay-based first attempt at the (now-removed) boot
splash rendered correctly on real hardware but was invisible to the
screenshot tool, which is this project's primary way of verifying UI on
real hardware without a physical tap. Building the disclaimer (and, while
it existed, the splash) as its own full `lv_screen` - loaded via
`lv_screen_load()` in place of the dashboard, then swapped back on Accept -
keeps it a normal `lv_screen_active()` target and fully verifiable by the
existing tooling.

### 6. Dev-only `nav` command simulates the Accept tap

`display_ui_register_dev_nav_console()`'s `"disclaimer"` target loads the
disclaimer screen; `"disclaimer accept"` instead calls the exact same
`disclaimer_accept_cb()` a real tap invokes, persisting acknowledgement and
switching to the dashboard - the same precedent as the existing
`"wifi_password"` nav target, which simulates reaching that screen without
a real connect attempt. This let the full persist-gate-reshow flow (first
boot shows it, Accept persists and dismisses, plain reboot skips it, a
version bump re-shows it) be verified end-to-end over serial, with no
physical touch.

## Consequences

- `settings_store` gains a fifth sealed-blob domain
  (`disclaimer_settings_t`, `SETTINGS_DISCLAIMER_MAGIC`/`_VERSION`),
  following the exact seal/validate/init-default pattern as the other four
  - host-tested the same way (`components/settings_store/host_test/test_settings_codec_disclaimer.c`,
  14 checks).
- `main/display_ui.c` gains `s_dashboard_screen` (captured once in
  `display_ui_render()`) and `s_disclaimer_screen`, plus
  `build_disclaimer_screen()` / `show_disclaimer_screen()` /
  `disclaimer_accept_cb()`.
- No `CONFIG_LV_FONT_MONTSERRAT_28` or larger hero font is needed - the
  disclaimer reuses the dashboard's existing 12-20px type scale
  (`/dashboard-design`).

## Alternatives considered

- **Boot/branding splash screen**: implemented, hardware-verified, then
  explicitly reverted - the product direction is "disclaimer only," no
  app-branding wordmark screen.
- **`lv_layer_top()` overlay for the disclaimer/splash**: rejected once the
  screenshot-tool blind spot was discovered (decision 5) - an own-screen
  approach costs nothing extra and stays fully testable.
- **Separate disclaimer-text-version NVS field**: rejected as redundant
  with the firmware version, which already changes exactly when disclaimer
  text could (decision 4).
