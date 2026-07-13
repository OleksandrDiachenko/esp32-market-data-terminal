# 0012: Lazy Settings screen lifecycle (bounded LVGL working set)

## Status

Accepted (2026-07-12)

## Context

Boot investigation (triggered by a user-reported "long white screen at
startup") measured `display_ui_render()` - which built all 16
screens/sub-screens (dashboard + 13 Settings sub-screens + status bar +
disclaimer) synchronously, once, at boot - taking **~17.4 seconds**, with
the panel's backlight already on the whole time (fixed separately in the PR
this ADR follows: reordering `board_jc4880p443c_backlight_on()` to after
render). Per-screen instrumentation isolated the cost:

| Screen | Baseline build time |
|---|---|
| `build_wifi_password_screen` | **8.37 s** |
| `build_watchlist_add_screen` | **2.23 s** |
| `build_date_format_screen` | 1.02 s |
| `build_time_zone_screen` / `build_display_screen` | 0.86 s each |
| `build_region_screen` | 0.58 s |
| `build_wifi_screen` | 0.55 s |
| remaining 8 screens | 0.2-0.4 s each |
| **Total** | **~17.4 s** |

Two concrete, root-caused bugs accounted for the two outliers (~10.6 s of
the ~17.4 s):

1. **Redundant keyboard button-matrix rebuilds.** Both keyboard-owning
   screens called `lv_keyboard_set_map()` four times (once per mode:
   lower/upper/symbols-1/symbols-2) at build time. LVGL's
   `lv_keyboard_update_map()` unconditionally rebuilds the *currently
   active* mode's full button matrix on every call, regardless of which
   mode was just registered - so four calls rebuilt the same LOWER-mode
   matrix four times over. Worse, both screens' own mode-switch event
   handlers (`wifi_keyboard_event_cb()`/`watchlist_add_keyboard_event_cb()`)
   already call `lv_buttonmatrix_set_map()` directly with the correct
   static array on every runtime mode switch, bypassing LVGL's *global*
   `kb_map[]`/`kb_ctrl[]` table entirely (a pre-existing, well-documented
   necessity - see the comment above `WIFI_KB_MODE_SYM_1`: that table has no
   per-object storage, so two keyboards sharing `LV_KEYBOARD_MODE_TEXT_LOWER`
   would silently clobber each other's map otherwise). The four build-time
   registrations were therefore never read back by anything - pure waste.
   Fixed by registering only the initially-visible mode, via one direct
   `lv_buttonmatrix_set_map()`/`_set_ctrl_map()` call (the same pattern the
   event handlers already use). Measured: `build_wifi_password_screen`'s
   keyboard construction dropped from 8.34 s to 25 ms.
2. **A one-time "first complex widget" tax.** With the keyboard fixed,
   `build_wifi_password_screen` still cost ~8.3 s - all of it in the
   screen's *first* `lv_textarea_create()` call specifically (isolated via
   further sub-function timing: 5.9 s in `lv_textarea_create()` alone, 269
   ms in the subsequent `style_dark_textarea()`). The second textarea on
   the same screen cost ~1.2 s; `build_watchlist_add_screen`'s own textarea
   (built later, after the tax had already been paid once) cost only
   low-hundreds of ms. This reads as a one-time construction/cache cost for
   a widget class the dashboard and earlier Settings screens never touch
   (none of them use `lv_textarea`/`lv_keyboard`/`lv_roller`) - possibly
   related to `lv_textarea`'s cursor-blink `lv_anim_t` (the first animation
   ever started in this app) interacting with this board's PSRAM-backed LVGL
   pool, though the exact mechanism inside LVGL/the allocator was not fully
   isolated beyond that.

Even after both fixes, the *remaining* 11 screens still cost ~5.3 s combined
purely from ordinary widget-count-proportional construction on a
PSRAM-backed pool (`CONFIG_LV_MEM_SIZE_KILOBYTES=512`, relocated there by a
prior phase - see decision 2 below) - meaningful work the user pays at boot
for screens they may never visit that session.

## Decision

### 1. Settings sub-screens are built on first navigation, torn down on exit

`display_ui_render()` now builds only the dashboard (`s_rows_container`),
the status bar, and the always-resident Settings list (`s_settings_list`)
at boot. Each of the 13 Settings sub-screens (Time/Time format/Date
format/Time zones/Time zone cities/Region/Display/Wi-Fi/Wi-Fi
password/Watchlist manage/Watchlist add/Updates/About) is built the first
time the user navigates into it (`ensure_settings_view_built(view)`, a
switch over `settings_view_t` that no-ops if the screen's root handle is
already non-NULL) and **destroyed** (`destroy_settings_view(view)`,
`lv_obj_delete()` + nulling every one of that screen's static handles) the
moment the user navigates away from it - `show_settings_view()` destroys
whichever sub-screen was previously showing before building/showing the new
one, and `set_active_screen(WATCHLIST)` (leaving Settings entirely) does the
same. At most one lazily-built sub-screen (plus the resident
`s_settings_list`) exists in memory at any moment - this is a genuine
bounded-working-set design, not just deferred construction: peak LVGL pool
usage is now `dashboard + status bar + whichever single sub-screen is
open`, never the sum of all 16.

This is the standard LVGL "screen-per-view" pattern (create-on-enter,
destroy-on-exit), not a novel abstraction - the codebase already used it
successfully for a smaller case (Settings > Display > Night mode's
start/end time picker, decision 0011's `night_time_open()`/
`night_time_msgbox_delete_cb()`), which this phase generalizes to the 13
top-level Settings sub-screens.

### 2. The PSRAM-backed pool from the prior Wi-Fi-crash fix is kept, not reverted

A prior fix (`82b1352`, "Move LVGL pool to PSRAM to fix Wi-Fi-nav
crash/freeze/dead-touch") relocated LVGL's 512 KB object pool from internal
RAM to PSRAM specifically because the *sum* of every screen's widgets
(dashboard + a busy Wi-Fi scan's ~2 KB/network + everything else, all
resident simultaneously) could exceed what internal RAM could spare. That
crash's root cause - too much simultaneously-resident LVGL state for the
available pool - is structurally addressed by decision 1 above (bounded
working set), which raised the question of whether the pool could move back
to faster internal RAM now that PSRAM's slower allocation cost is what
motivated this whole investigation.

Decision: **keep the PSRAM pool.** Two reasons:
- Decision 1 alone already exceeds every stated target (see Consequences) -
  moving the pool is an *optimization* with no unmet requirement driving it,
  not a fix.
- Reverting it re-opens exactly the failure mode `82b1352` closed, verified
  against the standing regression case in
  `docs/debugging/wifi-nav-pool-exhaustion.md` (a 26-synthetic-AP Wi-Fi list
  crashed a 128 KB internal-RAM pool deterministically). Re-litigating that
  trade-off isn't warranted by a boot-time problem decision 1 already
  solves on its own.

If a future phase needs to shave further milliseconds off first-entry
times, revisiting pool placement is easy to reconsider in isolation - it's
one `sdkconfig.defaults` value and one CMake hook
(`lvgl_ext/lv_psram_pool.h`), decoupled from the lifecycle change here.

### 3. A one-time boot-time "warm-up" absorbs the first-complex-widget tax

Rather than let whichever screen the user opens first (most likely Wi-Fi's
"Add Network" or Watchlist's "Add symbol", both textarea/keyboard screens)
silently eat the ~6-8 s one-time tax from finding #2 above, `display_ui_render()`
calls a new `warm_up_lvgl_widget_classes(screen)` immediately after LVGL
comes up, before building even the dashboard: a hidden, immediately-deleted
scratch object holding one `lv_textarea`, `lv_keyboard`, and `lv_roller`
(the latter defensively, for Night mode's picker - no equivalent spike was
observed for it in testing, but it's likewise untouched by the
dashboard/status bar). This pays the tax once, deterministically, during
the same dark-backlight boot window the reordering fix already covers -
not on a random future tap. Measured cost: 127 ms (not 6-8 s - whatever
made the very first `lv_textarea_create()` slow in the original eager-build
sequence did not reproduce when nothing else had been allocated yet;
placing the warm-up first rather than isolating *why* the number changed
was judged sufficient once it was confirmed empirically low and stable).

### 4. Six helper functions that touch a sub-screen's widgets before `show_settings_view()` now call `ensure_settings_view_built()` first

`watchlist_manage_rebuild()`, `updates_screen_reset()`,
`watchlist_add_screen_reset()`, `wifi_password_screen_set_ssid()`, and
`show_time_zone_cities()` are all called by their screen's entry-point
click handler *before* `show_settings_view()` runs (e.g.
`wifi_ap_click_cb()` calls `wifi_password_screen_set_ssid()` then
`show_settings_view(SETTINGS_VIEW_WIFI_PASSWORD)`) - each now starts with
`ensure_settings_view_built(<its own view>)` so the widgets it immediately
touches are guaranteed to exist. Two further functions
(`wifi_menu_hide()`, `region_refresh_marks()`) can run while a *different*
view is active (a defensive call on Wi-Fi screen entry; an auto-region
recompute triggered from a time-zone pick elsewhere in Settings) and gained
a NULL guard on their own screen's root handle instead, since there's
nothing to update if that screen was never built or was already torn down.

A **regression bug found during this phase's own testing**: `cmd_nav`'s
`wifi_password` target called `lv_textarea_set_text(s_wifi_password_input, "")`
*before* `wifi_password_screen_set_ssid()` (which is what actually ensures
the screen exists) - a dereference of a NULL handle that hung the device
hard enough that every subsequent `nav` command timed out until a manual
reset. Fixed by reordering; this is exactly the class of bug an exhaustive
before/after widget-touch audit (see the six functions above) exists to
catch, and this one slipped through until physical testing caught it -
underscoring why the full 15-target `nav` sweep plus re-entry cases (below)
is part of this phase's verification, not just a spot check.

### 5. `update_wifi_screen()`'s diff-cache is now file-level, reset on Wi-Fi screen teardown

`update_wifi_screen()` skips rebuilding `s_wifi_list` when the Wi-Fi
environment hasn't changed since the last tick, tracked via a
function-local `static bool s_wifi_list_built` plus file-level
`s_wifi_rendered_rows`/`s_wifi_rendered_count`. Promoted `s_wifi_list_built`
to file-level so `destroy_settings_view(SETTINGS_VIEW_WIFI)` can reset it
(along with `s_wifi_rendered_count`) on teardown - otherwise a freshly
rebuilt (empty) `s_wifi_list` on re-entry would compare "unchanged" against
the stale pre-teardown cache and never render anything until the Wi-Fi
environment itself changed. `watchlist_manage_rebuild()`,
`watchlist_add_screen_reset()`, and `updates_screen_reset()` were audited
for the same class of staleness risk and confirmed already
unconditional-refresh-on-entry by design (their own doc comments already
say so) - no equivalent fix needed there.

## Consequences

- **Boot time**: `display_ui_render()`'s render-only cost (warm-up +
  dashboard + status bar) measured **~1.1 s** (127 ms warm-up + 955 ms
  dashboard/status bar), and total time from touch-controller-ready to
  `"Display UI started."` measured **~1.4 s** - down from ~17.4 s
  (render alone) / ~19 s (including the backlight-ordering issue this
  followed). Both are well inside the ≤3.5 s target.
- **Per-screen first-entry time**: every one of the 13 lazy sub-screens,
  including re-entry after teardown, measured via `nav`'s `NAV_OK` round
  trip: 1-186 ms (worst case `watchlist_add`, previously 2.23 s;
  `wifi_password`, previously 8.37 s, now 114 ms) - all comfortably inside
  the ≤800 ms target with no per-screen exceptions needed.
- **No Wi-Fi-crash regression**: 150 cycles (450 total navigations) of
  `wifi` -> `watchlist_manage` -> `settings` via the dev console, the same
  class of stress that reproduced `82b1352`'s bug in 8-13 navigations
  originally, completed with zero failures.
- Every `build_*_screen()` function needed a forward declaration
  (`ensure_settings_view_built()`/`destroy_settings_view()` are defined
  before any of them, since `show_settings_view()` - itself defined early,
  right after the dashboard row-update code - needs both). Two file-level
  statics declared later in the file
  (`s_wifi_menu_backdrop`/`_card`/`_ssid_label`) needed the same treatment;
  `s_date_format_checks[]` (sized by `DATE_FORMAT_COUNT`, only known once
  `s_date_formats[]` is declared) instead got a small
  `destroy_date_format_view()` helper defined next to the array and
  forward-declared for `destroy_settings_view()` to call.
- `main/display_ui.c` grows by roughly the size of
  `ensure_settings_view_built()`/`destroy_settings_view()` (two ~110-line
  switch statements covering the 13 views) - the trade-off for a codebase
  that already leans on explicit switch statements throughout
  (`show_settings_view()` itself, `cmd_nav()`) rather than
  function-pointer/table-driven indirection, kept consistent here.
- Any future 14th Settings sub-screen must remember to add itself to *four*
  places (the two lazy-lifecycle switches, `build_*_screen()`'s forward
  declaration, and the boot-render call list if it should ever be eager) -
  a real ongoing cost of this pattern, mitigated only by how mechanical and
  localized each addition is.

## Alternatives considered

- **Keep eager boot construction, just fix the two outlier bugs (decision 4's
  keyboard fix + the textarea warm-up)**: seriously considered mid-investigation
  once those two fixes alone cut ~10.6 s of the ~17.4 s. Rejected because
  the user explicitly asked for the full lifecycle refactor after seeing
  that data, and because it leaves ~5.3 s of avoidable boot-time cost on
  the table (11 screens nobody may visit this session) with no bound on
  peak memory - the two point-fixes address *this session's* measured
  outliers, not the general "the app resident-builds everything it might
  ever show" shape of the problem.
- **Revert the PSRAM pool back to internal RAM now that decision 1 bounds
  the working set**: considered (this ADR's decision 2) and rejected as an
  unnecessary trade against a real, already-fixed crash, given decision 1
  alone already exceeds every target.
- **Lazily build only the two worst offenders (Wi-Fi password, Watchlist
  add) and leave the rest eager**: rejected for the same reason as the first
  alternative - narrower, and doesn't bound peak memory usage the way
  building/tearing down every sub-screen does.

## Amended 2026-07-13: re-audit after a temporary revert, plus baseline profiling

This decision, and the PSRAM pool move it built on (`82b1352`), were briefly
reverted locally (branch `revert/post-exit-hit-area-fixes`, commits
`46a6dc8`/`ced4699`/`527017c`) while re-investigating the original
crash/WDT reports with fresh backtraces. That investigation reconfirmed the
same root cause this ADR and `82b1352` already fixed (LVGL pool exhaustion
during Wi-Fi-list construction), found nothing that changes decisions 1-5
above, and the revert was undone: this ADR's lifecycle plus the PSRAM pool
are both still in effect on `main`. Two things came out of that pass worth
recording here rather than in a separate ADR:

**A latent bug in the lifecycle itself, fixed.** `schedule_watchlist_manage_rebuild()`
(added by `82b1352` as hardening, kept through this ADR) defers the
Watchlist Manage rebuild via `lv_async_call()` so a Remove/drag event
doesn't tear down the row indev still points at - but `destroy_settings_view()`
never cancelled that pending call. Remove/drag followed by an instant Back
left the async call queued against an already-destroyed screen; it later
fired, silently rebuilding (and re-showing behind whatever view was now
active) a Watchlist Manage the user had already left - an orphaned
sub-screen violating this ADR's core invariant (at most one lazily-built
sub-screen resident). Fixed by cancelling the pending call in
`destroy_settings_view()`'s `WATCHLIST_MANAGE` case, plus a
belt-and-suspenders guard in the async callback itself that no-ops if that
view isn't active. Verified via a temporary console hook reproducing the
exact race: `watchlist_manage_screen` stayed `ALIVE` after Back on the
unpatched build, `NULL` with the fix.

**Baseline hardware profiling, with one number worth flagging.** New
on-demand (`memlog` console command) and optional periodic
(`CONFIG_UI_DIAGNOSTICS`) instrumentation was added to measure LVGL
pool / internal-RAM / PSRAM headroom directly, rather than relying on the
one-off measurements above. Confirmed on-device:

- `resident_subscreens` (a live count of the 13 lazy sub-screen root
  pointers) never exceeded 1 across 150+ nav-stress cycles and a full sweep
  of every Settings view, including re-entry - decision 1's invariant holds
  in practice, not just by code inspection.
- Idle/per-screen LVGL pool usage with a normal Wi-Fi environment (3 real
  APs) and the Watchlist at its 10-symbol cap: 32-51% used, 60-85 KB free
  of the 512 KB pool - consistent with this ADR's and `82b1352`'s
  measurements.
- **The documented worst case is tighter than the historical single
  measurement suggested.** `82b1352`'s validation quoted a padded 26-row
  Wi-Fi list building at "used=30%, free=357KB". Re-measured here with the
  current code and a full `WIFI_DISPLAY_ROWS_MAX` (32) synthetic rows: the
  pool reaches **~92% used, ~9-10 KB free, ~7-8 KB biggest free block** -
  roughly an order of magnitude less headroom than the earlier number,
  most likely reflecting cumulative pool pressure from other features added
  since (Phase 15/16 screens, chart widgets, etc.) rather than a change in
  Wi-Fi-row cost itself. This was stress-tested directly: 25 full cycles of
  build-the-32-row-list -> navigate to Watchlist Manage -> back to Settings,
  each cycle re-confirming the ~92%/9-10KB state, zero crashes/WDT/errors,
  no downward drift across cycles (i.e. no leak) - so the current pool
  still comfortably survives the worst case, but with materially less
  margin than previously documented. This number should be the basis for
  any future `ui_mem_can_build()`-style low-memory guard's threshold (not
  the older 30%/357KB figure), and is a data point in favor of eventually
  reducing Wi-Fi/Watchlist row object-weight if further features add
  meaningfully to baseline pool usage.
