# Testing

## Current Validation
- `idf.py build`
- `make -C components/wifi_manager/host_test test` — host-side tests for
  the Wi-Fi connection policy state machine and profile blob codec (pure
  C, no ESP-IDF, built with plain gcc + ASan/UBSan). Not yet wired into CI
  (see roadmap Phase 8).

## Planned Tests
- host-side parser tests
- hardware tests are manual for now (see `docs/validation/`)