# Debug Report: Watchlist "Add symbol" total UI freeze near the 10-symbol cap

## Symptom

Adding watchlist symbols (typically noticed after two in a row) eventually leaves the
device completely unresponsive to touch - no crash, no reboot, no log output. The
screen stays on showing its last-rendered frame.

## Environment

- Date: 2026-07-07/08
- Board: JC4880P443C_I_W
- Target: esp32p4
- ESP-IDF: v6.0.1
- Port: /dev/cu.usbmodem101 (secondary/log-only console - see
  `reference_jc4880p443c_console_uart_wiring` project memory; no interactive REPL
  reachable over this port, so the repro itself had to be driven by hand on the
  physical touchscreen while a `pyserial` script captured the log)
- Watchlist size at the time of the original report: growing past ~7-9 of the 10
  possible symbols (`SETTINGS_MAX_WATCHLIST` = 10)

## Steps To Reproduce

1. Start from a watchlist already fairly full (7+ symbols).
2. Watchlist > Manage > Add symbol > type a valid Binance pair > Search > Add to
   watchlist.
3. Immediately repeat step 2 for a second symbol.
4. Try to interact with the device - no response to any touch.

## Expected Result

Both symbols are added, the dashboard shows them, and the UI stays responsive.

## Actual Result

Some time after the newly-added row's first successful REST sync (observed up to
~18s later in one capture), the device stops responding entirely. No crash log, no
reboot - `CONFIG_ESP_TASK_WDT_PANIC` is not set, so the watchdog logs and continues
rather than resetting the device.

## Investigation

### Initial hypothesis (disconfirmed)

The two most recent watchlist-related commits (`84bda12` add Add-symbol screen,
`587affa` live REST re-sync + WS SUBSCRIBE/UNSUBSCRIBE on edits) both introduce
blocking network calls that, for the first time in this codebase's history, can run
concurrently after boot (UI-task ticker fetch during "Search", sync-task's REST
klines fetch for a newly-added symbol, WS-task's SUBSCRIBE frame send). This project
has documented ESP-Hosted/SDIO instability under concurrent network load before (see
`sdkconfig.defaults`'s "RX streaming mode stays off" comment,
`app_state_ws_task.c`'s `wait_for_wifi_connected()` doc comment, and the SDIO
`tlsf_free` crash in `docs/validation/app-state-runtime-hardware-test.md`), so this
was the leading hypothesis going into hardware testing.

Temporary instrumentation (`ESP_LOGI` timing markers + `esp_task_wdt_add()`/
`_delete()` around all three candidate blocking calls, plus an independent 1Hz
heartbeat task) was added and flashed. On reproducing the freeze, the captured log
showed **all three instrumented network calls completed cleanly** (each logged a
"done" line) well before the freeze, and the heartbeat kept ticking throughout -
ruling out both a genuine cross-task deadlock and a scheduler-wide lockup.

### Confirmed root cause

`CONFIG_ESP_TASK_WDT` is already enabled (non-panicking, 20s timeout,
`sdkconfig`). It fired ~18s after the last application log line, reporting:

```
E task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E task_wdt:  - IDLE0 (CPU 0)
E task_wdt: Tasks currently running:
E task_wdt: CPU 0: taskLVGL
E task_wdt: CPU 1: IDLE1
Backtrace: 0x480643e6:0x4ff3d9b0 0x48064b24:0x4ff3d9f0 0x4802d3d8:0x4ff3db20 0x4802f06c:0x4ff3db30 0x48058fe8:0x4ff3db40 0x480590a8:0x4ff3db60 0x480370fa:0x4ff3db70 0x4811bc32:0x4ff3db80
```

`taskLVGL` is shown **currently running** (busy, not blocked on anything) - decoding
the backtrace against the exact flashed ELF (`riscv32-esp-elf-addr2line`) gives:

```
new_points_alloc            lv_chart.c:1885   (LV_ASSERT_MALLOC right after lv_realloc())
lv_chart_set_point_count    lv_chart.c:159
update_row                  main/display_ui.c:462
update_timer_cb             main/display_ui.c:643
lv_timer_exec / lv_timer_handler
lvgl_port_task
```

`update_timer_cb` is the dashboard's 1Hz refresh timer, which calls `update_row()`
for every watchlist row; the first time a row's kline count actually changes (i.e.
its first successful REST sync), `lv_chart_set_point_count()` reallocates that
chart's ~1.15KB (288 x int32_t) point buffer via `lv_realloc()`.

LVGL's internal allocator uses a **fixed-size static pool**
(`CONFIG_LV_MEM_SIZE_KILOBYTES=64`, `LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=0` - no
growth), separate from the system heap. `lv_mem_monitor()` measured only **~11.4KB
free (82% used) with 9 of 10 possible watchlist rows built** - even at a clean boot
with no runtime churn. Adding a 10th row's chart, or any further growth from
per-row objects across the app's several eagerly-built screens, exhausts it:
`lv_realloc()` returns `NULL`, `LV_ASSERT_MALLOC` (`CONFIG_LV_USE_ASSERT_MALLOC=y`)
fires, and `LV_ASSERT_HANDLER` (`while(1);` by default) traps the LVGL task in a
**permanent busy-loop**. Since `CONFIG_LV_USE_LOG` is *not* set, the assert's own
`LV_LOG_ERROR("... Out of memory")` message never prints - explaining the total
silence before the freeze. `taskLVGL` also owns touch-input processing
(`esp_lvgl_port`'s single task services both rendering and the touch indev under
`lvgl_port_lock()`), so trapping it here freezes the whole UI deterministically,
with no other symptom.

### Why "the second add" specifically

Not an inherent 2-vs-1 race: the pool's headroom is a function of **total row
count**, and the reported repro session happened to start from an already
substantially populated watchlist, so "add 2 more" reliably pushed the count into
the pool's exhaustion zone. A user starting from an empty watchlist and adding
exactly 2 symbols would not reproduce this on its own.

### Ruling out a leak (user-raised concern)

Before accepting "just make the pool bigger" as durable rather than delaying the
same failure, `lv_chart_destructor()` (`lv_chart.c:859-876`) was checked and
confirmed to `lv_free()` each series' point buffer on `lv_obj_delete()` -
`destroy_rows()`/`rebuild_rows_if_needed()` already deletes every row before
rebuilding, so there's no leak in the row-teardown path by inspection. This was then
verified empirically: with the pool doubled to 128KB, `lv_mem_monitor()` was logged
every 5s while manually cycling the watchlist through roughly ten full add/remove
sequences spanning 3 to 10 rows over ~5 minutes. `free_size` at a given row count
consistently returned to within a few hundred bytes of its prior value at that same
count (e.g. rows=9: 67132 -> 66808 -> 66768 -> 66768, settling rather than trending
down) - fragmentation noise that stabilizes, not a monotonic leak. The fix is
durable.

## Root Cause

LVGL's internal memory pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`, default 64KB, no
expansion configured) is too small for this app's dashboard once the watchlist
approaches its 10-symbol cap - each row's chart needs a real allocation the first
time it syncs. Exhaustion trips LVGL's built-in out-of-memory trap
(`LV_ASSERT_MALLOC` -> `while(1);`), which runs on the same task that services
touch input, producing a silent, total, unrecoverable freeze. `LV_USE_LOG` being
disabled meant the assert's own diagnostic message never reached the log, which is
why this looked like an unexplained hang rather than a reported allocation failure.

## Fix

- `sdkconfig.defaults` / `sdkconfig`: `CONFIG_LV_MEM_SIZE_KILOBYTES` 64 -> 128.
  Measured system-level internal free heap at the time was ~142KB (steady state,
  Wi-Fi/SDIO/display all up) outside this pool, so doubling it leaves comfortable
  margin on both sides (~75KB+ free inside the pool at 9-10 rows, ~78KB still free
  system-wide).
- Separately, `app_state_ws_task.c`'s `xQueueAddToSet()` calls had their return
  values silently discarded; FreeRTOS requires a queue to be empty when joining a
  set, and `market_data_ws_client_start()` began receiving ticks before the
  queue-set registration ran, so a tick arriving in that window could silently and
  permanently orphan the update queue from the set. Fixed by splitting
  `market_data_ws_client_start()` into `market_data_ws_client_create()` (builds the
  client and its queue, no data can arrive yet) and `market_data_ws_client_connect()`
  (starts the connection), and reordering `ws_task_fn()` to create -> join the queue
  set (now provably empty) -> connect, with both `xQueueAddToSet()` results checked.
  This was a real, confirmed bug found during the investigation but not the cause of
  the reported freeze (not tied to watchlist size, and the connect path completed
  cleanly in the captured freeze log).

## Validation

- Host tests for `market_data_ws_client` (`components/market_data_ws_client/host_test/`):
  42/42 checks passed unchanged after the create/connect split.
- Hardware: with the 128KB pool, cycled the watchlist through ~10 full add/remove
  sequences across the 3-10 row range over 5 minutes (a substantially more
  aggressive repro than the original report) with no freeze; `lv_mem_monitor()`
  confirmed stable, non-leaking headroom throughout (see above).
- Hardware: final combined build (memory fix + queue-set fix) boots cleanly -
  `WebSocket connected` logs with no `Queue set setup failed` warning and no task
  watchdog trigger.

## Follow-up

- No further action planned. If the watchlist limit is ever raised above 10, or more
  eagerly-built screens/widgets are added, re-measure `lv_mem_monitor()` headroom at
  the new maximum before shipping - the pool is still a fixed ceiling, not
  self-expanding (`LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=0`).
- Consider enabling `CONFIG_LV_USE_LOG` (routed to `ESP_LOG`) in a future pass so an
  LVGL-internal assert failure is never silent again, independent of this specific
  bug.
