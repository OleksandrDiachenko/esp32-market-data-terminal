# Testing

## Current Validation
- `idf.py build`
- `make -C components/wifi_manager/host_test test` — host-side tests for
  the Wi-Fi connection policy state machine and profile blob codec (pure
  C, no ESP-IDF, built with plain gcc + ASan/UBSan). Runs in CI as the
  `host-tests` job in `.github/workflows/build.yml`.
- `make -C components/settings_store/host_test test` — host-side tests for
  the display/symbols/locale settings blob codec (seal/validate,
  corruption and out-of-range rejection; pure C, same gcc + ASan/UBSan
  setup). Runs in the same `host-tests` CI job.

## Planned Tests
- host-side parser tests
- hardware tests are manual for now (see `docs/validation/`)