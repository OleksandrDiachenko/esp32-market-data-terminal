# Testing

## Current Validation
- `idf.py build`

### Host-tested (pure logic)
Every component below has a `host_test/` directory: a plain `Makefile`
(`gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined`, no
Unity/CMake) compiling each pure-C source file with no ESP-IDF includes.
Together these cover every parser and every state machine in the codebase.

- `make -C components/wifi_manager/host_test test` — host-side tests for
  the Wi-Fi connection policy state machine and profile blob codec (pure
  C, no ESP-IDF, built with plain gcc + ASan/UBSan). Runs in CI as the
  `host-tests` job in `.github/workflows/build.yml`.
- `make -C components/settings_store/host_test test` — host-side tests for
  the display/symbols/locale/api_region settings blob codec (seal/validate,
  corruption and out-of-range rejection; pure C, same gcc + ASan/UBSan
  setup). Runs in the same `host-tests` CI job.
- `make -C components/market_data_client/host_test test` — host-side tests
  for the Binance REST client's pure logic: the query-string builder, the
  incremental JSON tokenizer (including token reassembly across arbitrary
  chunk boundaries), and the two streaming grammars (exchangeInfo symbol
  status, klines rows) covering valid/malformed/edge-case JSON. Pure C, same
  gcc + ASan/UBSan setup, no ESP-IDF dependency. Runs in the same
  `host-tests` CI job.
- `make -C components/market_data_ws_client/host_test test` — host-side
  tests for the WebSocket kline stream client's pure logic: the
  combined-stream URL builder, and the streaming grammar for one
  `@kline_1s` event (valid/malformed/non-kline/unknown-key/type-mismatch
  cases, plus the same chunk-boundary reassembly check as the REST parsers).
  Pure C, same gcc + ASan/UBSan setup, no ESP-IDF dependency. Runs in the
  same `host-tests` CI job.
- `make -C components/app_state/host_test test` — host-side tests for the
  retry/backoff/resync policy and the `@kline_1s` → 5m-candle merge
  aggregator (merge-in-place, append-with-eviction, stale/out-of-order
  ignore, volume/trade-count accumulation only on a closed 1s kline). Pure
  C, same gcc + ASan/UBSan setup, no ESP-IDF dependency. Runs in the same
  `host-tests` CI job.
- `make -C components/display_format/host_test test` — host-side tests for
  the price/number formatting used in the Watchlist (adaptive 4-8 decimal
  precision for sub-$1 prices, K/M/B/T abbreviation above the 9-integer-digit
  threshold) and the chart series normalization used to scale kline closes
  into a fixed-point axis range without flattening low-priced symbols'
  sparklines. Pure C, same gcc + ASan/UBSan setup, no ESP-IDF dependency.
  Runs in the same `host-tests` CI job.

### Not host-tested, and why
The remaining files in each of the above components (plus a few components
with no `host_test/` directory at all) are ESP-IDF/FreeRTOS glue that can't
run off-target without real hardware, network stacks, or NVS - see
AGENTS.md's testing philosophy: don't unit-test Wi-Fi, TLS, or the real
display directly. Checked individually; none have separable pure logic
worth extracting into a host-testable module.

| Component | Not host-tested | Why |
|---|---|---|
| `wifi_manager` | `wifi_manager.c`, `wifi_manager_slave_ota.c` | `esp_wifi`, `esp_event` |
| `wifi_manager` | `wifi_profile_store.c` | real NVS I/O (not pure) |
| `settings_store` | `settings_store.c` | NVS |
| `market_data_client` | `market_data_client.c`, `market_data_http.c` | `esp_http_client`, `esp_crt_bundle` |
| `market_data_ws_client` | `market_data_ws_client.c` | `esp-websocket-client`, `esp_crt_bundle` |
| `app_state` | `app_state.c`, `app_state_sync_task.c`, `app_state_ws_task.c`, `app_state_ota_task.c` | FreeRTOS task/event orchestration over the above |
| `ota_client` | `ota_client.c` | `esp_https_ota`, `esp_app_desc` |
| `time_sync` | whole component (no `host_test/` dir) | `esp_netif_sntp`, `xTaskCreate`, `setenv`/`tzset` - no separable pure logic |
| `board_jc4880p443c` | whole component | header-only pin/config constants, no `.c` files |
| `main/` | `display_ui.c`, `app_lifecycle.c`, `startup_diagnostics.c`, `dev_console.c`, `dev_screenshot_console.c`, `esp32-market-data-terminal.c` | LVGL/display, FreeRTOS app entrypoint, ESP-IDF chip/heap APIs |

## Static Analysis
- `cppcheck` — runs in CI (`cppcheck` job in `.github/workflows/build.yml`)
  against the same 15 pure-logic `.c` files the `host-tests` job already
  compiles with plain gcc (no ESP-IDF includes). Scoped deliberately: those
  files have zero ESP-IDF headers, so cppcheck needs no `$IDF_PATH` and
  produces no missing-include noise. `main/*.c` and the ESP-IDF-glue `.c`
  files in each component (e.g. `wifi_manager.c`, `market_data_client.c`)
  are out of scope for the same reason host tests don't compile them — they
  need `esp_wifi.h`/`esp_http_client.h`/NVS/LVGL headers unavailable on a
  plain `ubuntu-latest` runner. This job is blocking (`--error-exitcode=1`).
- `clang-format` — a `.clang-format` (repo root) codifies the project's
  existing style (LLVM base, Allman braces, 4-space indent, 120-column
  limit). The `format-check` CI job runs `clang-format --dry-run --Werror`
  across `components/` and `main/` but is **non-blocking**
  (`continue-on-error: true`): no `.clang-format` existed before this was
  added, so a full-tree dry-run hasn't been reviewed yet and would likely
  flag most of the codebase. The job surfaces drift on every PR without
  forcing a large reformat commit; flipping it to blocking is a deliberate
  future step once someone reviews and applies that diff.

## Planned Tests
- hardware tests are manual for now (see `docs/validation/`)