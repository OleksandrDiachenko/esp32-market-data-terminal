# Pre-Phase 17 code audit

Issue: [#81](https://github.com/OleksandrDiachenko/crypto-market-data-ticker/issues/81)  
Baseline branch: `main`  
Baseline commit: `2ca471f2d82607edaad74276cce997bffb28f229`  
Audit branch: `chore/pre-phase-17-code-audit`  
Date: 2026-07-13

## Reproducible baseline

| Item | Value |
|---|---|
| Target / framework | ESP32-P4 / ESP-IDF 6.0.2 |
| Release defaults | `sdkconfig.defaults` SHA-256 `1cd6109524a4b3e370a2852e8e97876546e9d388d3ac82dd635c68809d79c839` |
| Dependency lock | `dependencies.lock` SHA-256 `4cf32bf5106623421716ee30f4346d35f982b33c4032c1f1475f59d99d6cd233` |
| Partition table | `partitions.csv` SHA-256 `39a79f7f00f84445d78785a23400c2d608c9e0a8411df54a2ab2ab13063cac3c` |
| Baseline application | `0x1c3af0` bytes; `0x23c510` bytes (56%) free in each 4 MB OTA slot |
| Baseline bootloader | `0x5960` bytes; `0x6a0` bytes (7%) free before partition table |
| Build configurations | clean release defaults; local dev config with screenshot console and UI diagnostics |

The application-size values are from the last clean ESP-IDF 6.0.2 build on
the baseline commit. CI must publish the equivalent result for the final audit
commit before this gate closes.

## Findings

| Severity | Finding and evidence | Disposition / verification |
|---|---|---|
| P0 | Temporary `reseed_watchlist_after_schema_bump()` restored deleted symbols after reboot | Removed. Empty NVS watchlist is host-tested as valid; physical delete-last/reboot/add-back scenario remains required. |
| P0 | An empty watchlist previously prevented the WebSocket consumer from starting, so the first later add could not live-subscribe | Consumer now waits without a client, creates it on first add, keeps an existing client safely idle after removing the last symbol, and cleans partial client/queue creation failures. Firmware build plus hardware scenario required. |
| P0 | Sparkline handler used `LV_EVENT_DRAW_TASK_ADDED` without `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS` | Flag added before callback registration. Screenshot must confirm visible area fill. |
| P1 | `main/display_ui.c` was 6512 lines with about 178 local functions | First extraction moved symbol sanitizing, RSSI mapping, relative-time formatting and night-window calculation to pure `display_ui_logic` (30 ASan/UBSan checks). It remains 6448 lines/175 local functions; staged view extraction is a follow-up, not a risky audit-time rewrite. |
| P1 | Static analysis covered only 15 pure-C files; formatting was non-blocking | Pure scan includes `display_ui_logic`; compile-database scan covers 6 `main` and 30 component translation units; format gate is now blocking. |
| P1 | Partial Wi-Fi/app-state initialization failures leaked resources; internal Wi-Fi queue-full events were silent | Failure paths now release created timers/queues/mutexes/PSRAM and Wi-Fi internal command drops log their exact kind/error. Firmware build required. |
| P1 | Symbol search (about 0.5-2 s) and update check/install (install measured about 24 s) execute synchronously in LVGL callbacks | Existing behavior is explicit and uses immediate redraws, but it blocks input/rendering and couples teardown to transport lifetime. Move these operations to owned worker tasks with cancel/result messages before calling the UI layer release-ready. |
| P1 | Setup, architecture, testing and validation indexes described an older system/ESP-IDF version | Current maintainer docs now describe ESP-IDF 6.0.2, runtime ownership, quality gates and all validation reports. Historical reports retain the version actually used when recorded. |
| P1 | Claude-specific tracked guides duplicated durable project knowledge and referenced removed `ota_console.c` | Replaced by neutral `docs/development/` guides; `.claude/` is ignored. Historical validation mentions remain evidence and are labeled by their original date/context. |
| P1 | Physical regression/soak evidence does not yet exist for this audit commit | Open gate in `docs/validation/pre-phase-17-release-readiness.md`; Phase 17 must not start until it passes. |
| P2 | Bootloader has only `0x6a0` bytes (7%) headroom | Monitor on every release build. No partition offset change is justified in this audit because the current bootloader fits and OTA/NVS layout must remain stable. |
| P2 | `wifi_manager.c` is 1292 lines | Review shows one owner-task orchestration responsibility plus already-extracted policy/profile modules. No split solely for LOC; extract only newly identified pure/testable policy. |
| P2 | `dependencies.lock` pins registry versions/hashes but is not a machine-readable third-party license inventory | Project `LICENSE`/`NOTICE` exist and downloaded components carry license files. Generate a release SBOM/license inventory if distribution requirements grow; do not track regenerated `managed_components/`. |

No P0 item is considered closed by code inspection alone where the disposition
explicitly requires firmware or hardware evidence.

## Repository hygiene

- No tracked `sdkconfig`, build output, managed components, `.DS_Store`, ELF,
  map or firmware binaries were found.
- No tracked file exceeds 300 KB. The largest file is the identified
  `main/display_ui.c`; tracked PNGs are intentional hardware-validation
  artifacts.
- The only executable first-party tracked file is
  `tools/esp_hosted_slave/build_slave_fw.sh`.
- Current-tree and Git-history scans found no common GitHub/OpenAI/AWS token
  signatures. Pattern scans are defense in depth; repository settings should
  still keep platform secret scanning enabled.
- Relative Markdown-link scan is clean after moving the development guides.
- Direct ESP Component Registry versions/hashes are pinned. Project
  `LICENSE`/`NOTICE` and downloaded dependency license files are present; the
  ignored vendor tree remains reproducible from the lock file.
- `TODO/FIXME/TEMPORARY/remove once` review found historical, explicitly
  documented temporary hardware hooks only; none remain active in first-party
  C sources.

## Architecture review

`display_ui.c` owns LVGL screen construction, lazy lifecycle, widget callbacks,
state-to-view composition and development navigation. Its static mutable state
and callback lifetime make a one-shot split high risk. The safe order is:

1. freeze screenshot/timing/memory evidence;
2. extract pure catalog/format/composition helpers with host tests;
3. split one view family per PR while preserving `display_ui.h`;
4. rerun every affected screenshot, re-entry and stress scenario.

`wifi_manager.c` remains the single owner of Wi-Fi command/event orchestration.
Retry policy, profile codec and NVS storage are already separate, so its size
alone is not evidence of a broken boundary.

The follow-up is specified as `Refactor: staged extraction of display_ui view
families`, owner `OleksandrDiachenko`, with screenshot/timing/memory baselines,
one view family per PR, unchanged `display_ui.h`, host tests for pure helpers
and the 150-cycle stress test as acceptance criteria. GitHub issue creation was
attempted during this audit but the connected environment rejected the write;
until it is created, this remains an open P1 tracked by audit issue #81.

A second follow-up is specified as `Refactor: move UI-triggered network and
OTA operations off the LVGL task`, owner `OleksandrDiachenko`. Acceptance
criteria: worker-owned request/result state, bounded/cancellable symbol lookup,
OTA progress marshalled back under the LVGL lock, safe screen teardown, no
blocking transport call in an LVGL callback, and hardware validation of
offline/timeout/install paths. Its GitHub write is blocked by the same
environment limit and is likewise tracked as open under #81.

## Automated evidence

- All eight host-test suites pass under ASan/UBSan.
- Repo-wide first-party clang-format dry-run passes.
- Pure-C cppcheck passes with style checks.
- Compile-database cppcheck passes warning/performance/portability checks for
  all first-party ESP-IDF translation units represented by the database.
- `git diff --check` passes.

The local final firmware build could not be completed in the managed sandbox:
ESP-IDF's component manager calls macOS `sysctl()` via `psutil`, which the
sandbox denies. This is an environment limitation, not a green build result;
the clean GitHub Actions ESP-IDF 6.0.2 build remains mandatory.

## Exit gate

- [ ] GitHub CI builds the final audit commit without first-party warnings
- [ ] Final `idf.py size` and component-size report are attached
- [ ] No open P0 finding remains after hardware verification
- [ ] Every P1 is fixed or has a concrete follow-up issue and rationale
- [ ] `docs/validation/pre-phase-17-release-readiness.md` is marked Passed
- [ ] Only then change Phase 16.5 to Done and begin Phase 17
