# Debug Report: Crash / freeze / dead-touch during Settings > Wi-Fi navigation

## Symptom

While actively navigating the menus - repeatedly entering Settings > Wi-Fi and
Settings > Watchlist symbols, backing out with `<` as soon as it appears, all with
Wi-Fi scanning running - the device intermittently did one of three things:

1. **Crash** - `Guru Meditation Error` (Load / Store access fault), often while
   opening Watchlist symbols.
2. **Freeze** - the Wi-Fi screen stopped updating and touch went dead, no crash.
3. **Freeze + Task WDT** - same dead UI, plus `task_wdt` firing every 20s naming
   `taskLVGL` on CPU 0.

All three were intermittent and only ever showed up around Wi-Fi navigation.

## Environment

- Date: 2026-07-12
- Board: JC4880P443C_I_W
- Target: esp32p4 (rev v1.3 - i.e. pre-rev-v3)
- ESP-IDF: v6.0.2
- Port: /dev/cu.usbmodem101 (bidirectional USB-Serial-JTAG REPL - the dev
  `nav`/`screenshot` console is reachable here, see
  `reference_jc4880p443c_console_uart_wiring` project memory)

## Steps To Reproduce

Physical: rapidly navigate Wi-Fi <-> Watchlist symbols 20-30x while Wi-Fi scans,
in an RF environment with many APs in range.

Deterministic (no touch, via the dev console): drive `nav wifi` / `nav
watchlist_manage` / `nav settings` in a tight loop
(`tools`/scratch `repro.py`). Wi-Fi-only churn wedged in ~8-13 navigations;
watchlist-only churn ran 320 navigations clean.

## Expected Result

Navigation stays responsive; no crash, no WDT, touch stays live.

## Actual Result

The LVGL task crashed or wedged. Symbolized backtraces (against the running
`build/…​.elf`):

- Crash: `watchlist_row_click_cb -> watchlist_manage_rebuild ->
  build_watchlist_symbol_row -> lv_obj_set_style_text_color -> get_local_style(obj=NULL)`
  (a store/load to `NULL + offset`).
- WDT: `lv_display_refr_timer -> … -> lv_draw_label -> lv_draw_label_iterate_characters`
  spinning forever (taskLVGL never yields).

Both point at the same thing from different angles: an LVGL allocation that
should have succeeded either returned `NULL`/garbage (crash) or produced a
partially-built object the renderer then walked into a loop over (WDT).

## Investigation

### Initial hypothesis (disconfirmed)

First theory was a use-after-free from tearing down LVGL objects while they were
the active touch target (deleting a row from inside its own click handler, or the
per-tick Wi-Fi list rebuild deleting a row under a finger). A fix was written for
that class (defer the watchlist rebuild via `lv_async_call`; skip the Wi-Fi list
rebuild while a finger is pressed on it). It did **not** stop the bug - the WDT
freeze reproduced on the very next stress run, and, crucially, reproduced with the
**dev console driving navigation and no touch at all**. That ruled out
touch/teardown timing as the cause.

### Ruling out a leak

A temporary `lv_mem_monitor()` log around the Wi-Fi list rebuild showed
`lv_obj_clean(s_wifi_list)` frees correctly (6-8 KB per rebuild) and that
post-clean free is flat across dozens of rebuilds - no leak. But the same log
caught the pool at **used=94%, free=7448 bytes** just before a wedge in one rapid
run, versus a ~79% idle baseline.

### Confirmed root cause

The LVGL object pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`, then 128 KB, in internal RAM)
was **sized for the dashboard alone** (see `watchlist-add-freeze.md`, which bumped
it 64->128 KB for the row/chart widgets). The later Settings > Wi-Fi screen renders
**one ~2 KB widget subtree per network in range** (row + 4-bar signal icon + labels
+ optional lock/kebab = ~10 lv_objs). The rest of the UI already holds ~93 KB of the
128 KB pool, leaving only ~25 KB free - room for ~12 Wi-Fi rows. In a busy RF
environment (20-30 APs) the list overflows the pool mid-build.

Proven deterministically: temporarily padding the Wi-Fi list to 26 synthetic rows
crashed every time with a `Store access fault` in
`build_signal_icon -> lv_obj_create -> lv_obj_class_create_obj`, i.e. exactly the
allocation-failure site. LVGL's failure path here is catastrophic:
`LV_ASSERT_MALLOC` traps in an infinite `while(1)` on the LVGL task (silent freeze
-> Task WDT), or a near-full/fragmented pool hands back a bad block and faults.

This unifies all three symptoms as one bug (pool exhaustion) and explains the
intermittency (it scales with the number of nearby APs) and why watchlist-only
churn never reproduced it (no per-AP widgets).

## Root Cause

The LVGL builtin-malloc object pool was too small for the whole UI's worst case.
The Wi-Fi list's per-AP widget cost, on top of the dashboard, overflows a 128 KB
internal-RAM pool once enough networks are in range; LVGL's allocation-failure
behavior then crashes or infinite-loops the LVGL task.

## Fix

Relocate the LVGL pool to PSRAM and enlarge it to 512 KB, so worst-case whole-UI
demand (~160 KB) sits far below capacity and is out of the contended internal RAM
shared with esp_hosted / Wi-Fi / TLS.

A static PSRAM pool via `EXT_RAM_BSS_ATTR` does **not** link on this pre-rev-v3
ESP32-P4: the toolchain already runs with `-Wl,--enable-non-contiguous-regions`
(added unconditionally for P4 rev < v3), which discards several `.sbss` sections
once `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` is enabled. Instead, LVGL's builtin
TLSF is pointed at a **runtime** PSRAM allocation via its `LV_MEM_POOL_ALLOC` hook:

- `lvgl_ext/lv_psram_pool.h` - `static inline lv_psram_pool_alloc()` wrapping
  `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.
- Top-level `CMakeLists.txt` sets, as global build properties before `project()`,
  `LV_MEM_POOL_INCLUDE="lv_psram_pool.h"` and `LV_MEM_POOL_ALLOC=lv_psram_pool_alloc`
  (both object-like - CMake silently drops function-like `-D` macros), plus the
  header's include dir.
- `sdkconfig.defaults`: `CONFIG_LV_MEM_SIZE_KILOBYTES=512`.

`lv_mem_monitor()` still works (it's the same TLSF, just PSRAM-backed).

Also kept, as defensible independent hardening (not the root cause): deferring the
watchlist Manage rebuild out of its own input-event via `lv_async_call`, and
skipping the Wi-Fi list rebuild while a finger is pressed on the list.

## Validation

- Padded-26-row Wi-Fi list, which crashed reliably on the 128 KB pool, now builds
  at **used=30%, free=357 KB** of the 512 KB pool - no crash, no WDT.
- Idle pool sits at ~19% used / ~418 KB free (was ~79% of 128 KB).
- Rapid console-driven Wi-Fi/watchlist navigation runs clean.
- `app_state` host tests pass.

## Follow-up

- If a future screen pushes peak usage far higher, the pool can grow further at
  near-zero cost now that it's in PSRAM.
- Consider whether the LVGL assert handler should be made non-fatal (log + skip a
  widget) rather than `while(1)`-trap, so a future overflow degrades instead of
  freezing.

## Re-measured 2026-07-13 (see decision 0012's "Amended" section)

Re-ran this validation with the current tree and a full `WIFI_DISPLAY_ROWS_MAX`
(32, not 26) synthetic-row list: **used=92%, free=~9-10 KB, biggest free block
~7-8 KB** - materially tighter than the 30%/357KB figure above, most likely from
cumulative pool pressure added by later features rather than a change in
per-row cost. Stress-tested 25 full cycles at this exact state (rebuild the
32-row list, navigate away and back) with zero crashes/WDT and no drift. Still
safe, but treat the 30%/357KB number above as historical, not current - use
`memlog` (`CONFIG_DEV_SCREENSHOT_CONSOLE`) or `CONFIG_UI_DIAGNOSTICS` to
re-measure before relying on either figure.
