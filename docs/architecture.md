# Architecture

## Runtime overview

`app_main()` initializes persistent settings, board/display services, Wi-Fi,
time sync, runtime state and the LVGL UI. Hardware and network callbacks do not
mutate LVGL objects directly; `display_ui` reads snapshots and performs UI
updates while holding the LVGL port lock.

```text
JC4880P443C board support ──> LVGL display/touch/backlight ──> display_ui
ESP-Hosted ESP32-C6 ───────> wifi_manager ──> time_sync
                                             │
Binance REST ──> market_data_client ─────────┤
Binance WSS  ──> market_data_ws_client ──────┼──> app_state snapshots ──> display_ui
GitHub OTA   ──> ota_client ─────────────────┘
NVS          <── settings_store <──────────── UI/settings actions
```

## Module ownership

| Module | Responsibility | Important boundary |
|---|---|---|
| `board_jc4880p443c` | ESP32-P4 display, touch and backlight bring-up | Board-specific pins and drivers stay here |
| `wifi_manager` | One owner task for ESP-Hosted Wi-Fi state, profiles, scans and retry policy | Callers use snapshots/commands, not `esp_wifi` directly |
| `settings_store` | Versioned, CRC-sealed NVS records | Corrupt/old records fall back explicitly; empty watchlist is valid |
| `time_sync` | SNTP readiness and POSIX time-zone application | Network/TLS work waits for valid time |
| `market_data_client` | Blocking public Binance REST calls and streaming JSON parsing | Invoked by background orchestration, never by LVGL callbacks |
| `market_data_ws_client` | One WebSocket client, control frames and parsed kline update queue | Queue ownership follows create/stop lifecycle |
| `ota_client` | Manifest check and streaming HTTPS OTA | Uses certificate bundle and ESP-IDF rollback flow |
| `app_state` | Runtime snapshots plus REST/WS/OTA worker tasks | Sole bridge between services and UI state |
| `display_format` | Pure price/range/chart formatting | Host-testable; no LVGL dependency |
| `main/display_ui_logic` | Pure symbol/RSSI/time/night-window helpers | Host-testable; no ESP-IDF or LVGL dependency |
| `main/display_ui` | LVGL views, callbacks and lazy screen lifecycle | Public `display_ui.h` remains the stable UI boundary |

## Concurrency and lifetime

- `wifi_manager` serializes Wi-Fi operations in its owner task.
- `app_state` owns independent REST sync, WebSocket and OTA tasks and exposes
  copied snapshots to the UI.
- The WebSocket task remains alive for an empty watchlist and starts a client
  when the first symbol is added.
- Settings sub-screens are created on entry and destroyed on exit. Deferred
  callbacks must validate that their target screen still exists.
- LVGL access is made under the ESP LVGL port lock. Steady-state REST/WS/OTA
  work runs in background tasks. Two manual actions still perform synchronous
  network work from LVGL callbacks (symbol search and update check/install);
  the Phase 16.5 audit records moving them off the UI task as a P1 follow-up.

## Persistence and update compatibility

- NVS structures carry magic/version/CRC fields; schema changes require an
  explicit migration or documented default, never a temporary boot reseed.
- The partition table contains factory plus two 4 MB OTA slots. Firmware must
  fit each slot with documented headroom.
- Public headers and persisted layouts are compatibility boundaries. The
  Phase 16.5 extraction does not change `display_ui.h`, `wifi_manager.h`, NVS
  layouts or partitions.

Architectural decisions and hardware evidence live in
[decisions/](decisions/) and [validation/](validation/).
