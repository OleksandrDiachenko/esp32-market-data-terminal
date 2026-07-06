---
name: dashboard-design
description: Design system for this project's LVGL dashboard UI (main/display_ui.c) - dark palette tokens, type scale, layout constants, and LVGL implementation patterns (native-widget dark-theme restyling, sparkline area-fill technique, flag-stripping helpers). Use before building or reviewing any Watchlist/Settings screen so new UI stays visually consistent with what's already shipped, instead of re-deriving colors/sizes from scratch or drifting from the reviewed mockup.
---

# Dashboard design system (Phase 11 UI)

Snapshot as of the Phase 11 design-fidelity pass (2026-07-06), taken directly from
`main/display_ui.c` and the originally-reviewed HTML mockup. Code is the source of truth -
**grep for a constant/function name below before relying on it**; this file can drift as the UI
evolves and won't be auto-updated by every future change.

## When to use this

Before adding or editing any screen in `main/display_ui.c` (Watchlist rows, Settings list/rows,
Wi-Fi, Locale, future Manage/Add-symbol or Updates screens), or before reviewing such a change for
visual consistency. Also useful when starting a *new* chat session on this repo, since the design
decisions below came out of several rounds of user feedback and aren't otherwise written down in
one place.

## Color tokens

All defined as `#define COLOR_* lv_color_hex(...)` near the top of `display_ui.c`:

| Token | Hex | Use |
|---|---|---|
| `COLOR_INK` | `#0A0C10` | Screen background - near-black, not pure black |
| `COLOR_TEXT` | `#ECEEF2` | Primary text |
| `COLOR_MUTED` | `#6E7686` | Secondary/tertiary text: nav button ("Settings"/"Exit"), sub-screen back arrows, chevrons, descriptions, muted status |
| `COLOR_UP` | `#2FD481` | Positive price change, "on" switch track |
| `COLOR_DOWN` | `#FF5D6C` | Negative price change |
| `COLOR_WARN` | `#F2A93C` | Degraded/connecting/error states |
| `COLOR_HAIRLINE` | `#1B1F26` | 1px row/section dividers; also the Settings-row icon chip background |
| `COLOR_ACCENT` | `#47C9FF` | **Only** for: Settings-row icon chips (icon color inside the chip), primary CTA buttons (e.g. Wi-Fi password screen's "Connect"), textarea cursor. **Not** for nav text or back arrows - those are `COLOR_MUTED` (a corrected mistake; accent-blue nav text read as inconsistent against the mockup, which used muted gray there) |
| `COLOR_STATUSBAR_BG` | `#0F1216` | Bottom status bar background |

## Type scale

Montserrat only, sizes enabled via `sdkconfig.defaults` (`CONFIG_LV_FONT_MONTSERRAT_12/16/18/20`,
plus the LVGL-default 14). Role → size, as currently used in `build_row()`:

- Ticker symbol: 18
- Price: 20
- 24h range (min/max), Settings row description: 12 (range) / 12 (desc) - keep muted text small
- Change %, status bar clock: 16
- Status bar connection text, nav button, Settings row title, sub-screen header title: 14-16
  (check the specific label in code - not perfectly uniform, by design: title text reads larger
  than secondary labels in the same row)

## Layout constants

From the `#define`s near the top of `display_ui.c` - check these still match before reusing:

- `ROW_HEIGHT_PX` (76) - Watchlist row height
- `ROW_SIDE_COL_WIDTH_PX` (128) - left/right column width in a Watchlist row
- `STATUSBAR_HEIGHT_PX` (40) - bottom bar
- `SETTINGS_ROW_HEIGHT_PX` (108) - Settings list row (icon chip + title/desc + chevron)
- `SETTINGS_LIST_HEADER_HEIGHT_PX` / `SUBHEADER_HEIGHT_PX` (64 each) - top-level Settings header
  (no back arrow) vs. sub-screen header (back arrow + title) - same height, different content
- `SETTINGS_ICON_CHIP_PX` (40) - the rounded icon chip square in a Settings row
- `NAV_BUTTON_WIDTH_PX` (130) - fixed, so the bottom-bar nav button doesn't resize/shift between
  "Settings" and "Exit"

## Structural patterns

- **Settings row** = icon chip (rounded square, `COLOR_HAIRLINE` bg, `COLOR_ACCENT` icon) + a
  title/description column (title bold `COLOR_TEXT`, description small `COLOR_MUTED`, *live* not
  decorative - e.g. Wi-Fi row's description shows the actual connection state) + a chevron. See
  `build_settings_row()`.
- **Sub-screen header** = back arrow (`COLOR_MUTED`) + title, one shared height with the top-level
  Settings header (no back arrow there - it's the top level). See `build_subscreen_header()`.
- **Bottom nav button** is one button whose label/action changes with context ("Settings" on
  Watchlist, "Exit" anywhere in Settings) rather than two persistent tabs - see `nav_click_cb()` /
  `set_active_screen()`.
- Plain layout containers (rows, headers, chips) go through `lv_obj_remove_style_all()` +
  `make_plain_container()`, which strips the scroll/gesture/focus flags `lv_obj_create()` adds by
  default - left on, they can make a tap register as an aborted scroll instead of a click. Genuine
  scroll containers (e.g. the Wi-Fi network list) deliberately keep the scroll-related flags - see
  the comment at that call site for which flags are kept and why.
- Native LVGL widgets (`lv_switch`, `lv_textarea`, `lv_keyboard`) need explicit dark-theme
  restyling - LVGL's built-in default theme is light/blue and clashes with `COLOR_INK`. See
  `style_dark_switch()` / `style_dark_textarea()` / `style_dark_keyboard()`.
- Watchlist sparkline has a gradient area-fill under the line (`lv_chart` has no built-in fill for
  `LV_CHART_TYPE_LINE`) via an `LV_EVENT_DRAW_TASK_ADDED` hook, adapted from the vendored
  `managed_components/lvgl__lvgl/demos/widgets/lv_demo_widgets_analytics.c`. See
  `chart_draw_event_cb()`.
- Wi-Fi row signal-strength bars (`build_signal_icon()`) and lock icon (`build_lock_icon()`) are
  plain nested `lv_obj`/`lv_arc` composition, not a font glyph - no lock symbol exists in the
  vendored symbol font. `wifi_manager_ap_t.secured` (from the scan record's ESP-IDF `authmode`)
  drives whether the lock renders at all.
- Wi-Fi password keyboard uses a custom button map (`s_wifi_kb_map_lc`/`_uc` +
  `s_wifi_kb_ctrl_map`) via `lv_keyboard_set_map()` instead of LVGL's stock layout, to drop the
  cursor left/right keys and add an accent "Connect" key in their place (a literal "Connect" label
  bypasses the stock `lv_keyboard_def_event_cb`'s string-match dispatch, so
  `wifi_keyboard_event_cb()` replaces it, delegating everything except "Connect" back to the
  original handler). Ghost (shift/backspace/1#/space) vs. action ("Connect") key coloring uses the
  same `LV_EVENT_DRAW_TASK_ADDED` technique as the sparkline fill, keyed by
  `LV_BUTTONMATRIX_CTRL_CUSTOM_1`/`_2` instead of chart series data - see
  `wifi_keyboard_draw_event_cb()`.
  - **Gotcha that cost real debugging time**: `LV_EVENT_DRAW_TASK_ADDED` is gated behind
    `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS` (checked in `lv_draw_finalize_task_creation()`,
    `lv_draw.c`) - without that flag on the target object, the event never fires at all, silently,
    no matter how correct the handler is. `wifi_keyboard_draw_event_cb()`'s registration sets it
    explicitly. **`chart_draw_event_cb()` does not** - grep confirms the flag is set nowhere for
    `row->chart`, which means the sparkline gradient fill may never actually be rendering and the
    Watchlist charts could be plain lines today. Worth an on-device screenshot check before
    trusting that feature is live.

## Known, deliberate gaps (not bugs)

- Wi-Fi on/off toggle and "Add hidden network" from the original mockup are not implemented (no
  backing API / still an open design question).
- Wi-Fi row signal bars only reflect `rssi`; there's no live-updating signal animation, and the
  4-tier dBm thresholds in `wifi_rssi_to_bars()` are a reasonable approximation, not calibrated
  against real-world measurements.
- App-language switching (English/Ukrainian) was explicitly deferred - `locale_settings_t` has no
  UI-language field yet, only `time_24h`/`posix_tz`.
- The Wi-Fi password keyboard's "1#" key still switches to LVGL's stock `MODE_SPECIAL` layout
  (symbols, with cursor arrows) - only the TEXT_LOWER/TEXT_UPPER maps were customized, since the
  mockup never showed a symbols-entry state.
