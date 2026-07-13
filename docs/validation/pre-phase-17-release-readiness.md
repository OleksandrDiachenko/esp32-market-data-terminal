# Hardware validation: pre-Phase 17 release readiness

## Environment

- Date: pending
- Commit: pending
- Board: JC4880P443C_I_W
- Target: esp32p4
- ESP-IDF: 6.0.2
- Port: `/dev/cu.usbmodem101`
- Release config: `CONFIG_DEV_SCREENSHOT_CONSOLE=n`, `CONFIG_UI_DIAGNOSTICS=n`
- Diagnostic config: both development options enabled locally

## Gate checklist

- [ ] Release firmware flashed from clean defaults
- [ ] Boot to `Display UI started` <= 3.5 s
- [ ] Every lazy-screen first entry and re-entry <= 800 ms
- [ ] All 17 `dev_screenshot.py --nav` targets captured without rendering regressions
- [ ] Sparkline area fill visible (draw-task callback confirmed)
- [ ] Brightness slider and night window, including midnight span, verified
- [ ] Delete final symbol -> reboot -> list remains empty -> add symbol succeeds
- [ ] Ten-symbol watchlist receives REST bootstrap and live WebSocket updates
- [ ] Maximum visible Wi-Fi scan list remains responsive
- [ ] Offline/reconnect and API-region resync recover without reboot
- [ ] OTA check/install and rollback protection verified for release configuration
- [ ] 150 cycles / 450 navigations complete without crash or watchdog
- [ ] 60-minute soak complete with start/end heap, LVGL pool and stack snapshots

## Resource snapshots

| Point | Internal free/min | PSRAM free/min | LVGL used/free/largest | Lowest task stack | Notes |
|---|---:|---:|---:|---:|---|
| Start | pending | pending | pending | pending | |
| 30 min | pending | pending | pending | pending | |
| 60 min | pending | pending | pending | pending | |

## Result

Pending. This report must not be marked passed until the firmware build, flash
and physical observations above are recorded. A crash, allocation failure,
watchdog, unsafe stack watermark or unexplained monotonic memory drift is a P0
finding.
