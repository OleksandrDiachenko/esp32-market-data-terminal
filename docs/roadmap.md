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
Status: Planned

Note: depends on Phase 6's time sync — TLS certificate validation fails on
an unsynced clock (default epoch), independent of any Wi-Fi connectivity.

Acceptance criteria:
- [ ] Public market data endpoint selected
- [ ] HTTP/TLS timeout handling exists
- [ ] JSON parser handles success/error paths
- [ ] No API keys or secrets are required

### Phase 8: Runtime state + error handling
Status: Planned

Acceptance criteria:
- [ ] Runtime state model exists
- [ ] Recoverable errors are represented clearly
- [ ] Fatal errors are logged and documented

### Phase 9: Host-side tests + CI hardening
Status: Planned

Acceptance criteria:
- [ ] Host-testable modules identified
- [ ] Parser/state machine tests added
- [ ] CI runs host-side unit tests on push/PR

### Phase 10: Portfolio polish
Status: Planned

Acceptance criteria:
- [ ] README explains project value clearly
- [ ] Architecture docs are updated
- [ ] Screenshots/photos/log evidence are added
- [ ] Git history shows small reviewed changes