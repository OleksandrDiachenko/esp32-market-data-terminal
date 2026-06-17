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
Status: In progress

Acceptance criteria:
- [x] ESP32-P4 target configured
- [x] Flash/PSRAM baseline configured
- [x] Startup diagnostics pass on hardware
- [x] Hardware test notes documented

### Phase 2: Application lifecycle
Status: Planned

Acceptance criteria:
- [x] `app_main()` stays small
- [x] Application lifecycle module exists
- [x] Init/start failures return `esp_err_t`
- [x] Lifecycle logs are visible in monitor

### Phase 3: Display/UI skeleton
Status: Planned

Acceptance criteria:
- [ ] Display board layer is isolated
- [ ] Backlight/display init is documented
- [ ] Minimal UI renders on screen
- [ ] Display failure path is logged

### Phase 4: Network connectivity
Status: Planned

Acceptance criteria:
- [ ] Connectivity approach for ESP32-P4 + ESP32-C6 is documented
- [ ] Wi-Fi/hosted dependency decision is recorded
- [ ] Network init has timeout/error handling

### Phase 5: Market data client
Status: Planned

Acceptance criteria:
- [ ] Public market data endpoint selected
- [ ] HTTP/TLS timeout handling exists
- [ ] JSON parser handles success/error paths
- [ ] No API keys or secrets are required

### Phase 6: Runtime state + error handling
Status: Planned

Acceptance criteria:
- [ ] Runtime state model exists
- [ ] Recoverable errors are represented clearly
- [ ] Fatal errors are logged and documented

### Phase 7: Tests + CI
Status: Planned

Acceptance criteria:
- [ ] Host-testable modules identified
- [ ] Parser/state machine tests added
- [ ] Basic CI checks build or lint the project

### Phase 8: Portfolio polish
Status: Planned

Acceptance criteria:
- [ ] README explains project value clearly
- [ ] Architecture docs are updated
- [ ] Screenshots/photos/log evidence are added
- [ ] Git history shows small reviewed changes