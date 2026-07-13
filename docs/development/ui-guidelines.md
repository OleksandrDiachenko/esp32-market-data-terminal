# LVGL UI guidelines

This is a maintainer guide, not a second source of truth. Constants and helper
implementations in `main/display_ui.c` remain authoritative.

## Visual language

- Near-black screen/status backgrounds, light primary text and muted secondary
  text.
- Green/red are reserved for positive/negative market movement; amber is used
  for degraded or transitional states.
- Cyan accent is limited to primary actions, active controls and settings icon
  chips. Navigation/back labels remain muted.
- Settings rows use an icon chip, title/description column and chevron.
- Native LVGL switches, text areas, keyboards, sliders and rollers require
  explicit dark-theme styling.

## Interaction and lifecycle

- Reuse the shared plain-container/header/row helpers before adding one-off
  styles.
- Do not strip scroll flags from genuine list containers.
- Settings sub-screens are lazy: create on entry and destroy on exit. Async or
  deferred callbacks must verify their target still exists after teardown.
- Do not perform network or NVS work while holding the LVGL lock.
- Any `LV_EVENT_DRAW_TASK_ADDED` callback requires
  `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS` on its target. Verify custom drawing with
  a real-device screenshot.
- Preserve the public `display_ui.h` boundary when extracting helpers. Prefer
  pure C modules with host tests for formatting, mapping and validation logic.

## Review evidence

For UI changes, attach screenshots for every affected state, include first
entry and re-entry timing for lazy screens, and run the navigation stress test.
Record heap/LVGL-pool and task-stack observations when object count or callback
lifetime changes.
