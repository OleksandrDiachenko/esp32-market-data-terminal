# 0006: OTA firmware update via GitHub Releases

## Status

Accepted (2026-07-05)

## Context

Phase 10 (`docs/roadmap.md`) needs a way to update this project's own P4
firmware in the field, sourced from this repo's public GitHub Releases -
distinct from [0005](0005-esp-hosted-slave-ota.md), which updates the
ESP32-C6 *co-processor* firmware over SDIO. That ADR's partition
(`slave_fw`) and mechanism (`esp_hosted_slave_ota_*`) are specific to the
co-processor image; this ADR covers the host P4's own app image via the
standard ESP-IDF `esp_https_ota` component.

The current partition table (`partitions.csv`) has a single `factory` app
partition - there is no second slot to fail over to, and no `otadata`
partition for the bootloader to track which slot is active. Both are
prerequisites for any OTA mechanism, standard or otherwise.

## Decision

### Partition table: `otadata` + `ota_0`/`ota_1`, reusing already-unallocated space

```
# Name,     Type, SubType,  Offset,   Size, Flags
nvs,        data, nvs,      0x9000,   24K,
phy_init,   data, phy,      0xF000,   4K,
otadata,    data, ota,      0x10000,  8K,
ota_0,      app,  ota_0,    0x20000,  4M,
ota_1,      app,  ota_1,    0x420000, 4M,
nvs_keys,   data, nvs_keys, 0xD20000, 4K,   encrypted
wifi_cfg,   data, nvs,      0xD21000, 64K,
slave_fw,   data, 0x40,     0xD40000, 2M,
```

- `otadata` (8 KB, the size ESP-IDF requires) fills the previously-unused
  64 KB gap between `phy_init` and the old `factory` start - no existing
  partition moves.
- `ota_0` takes over `factory`'s exact old offset/size (`0x20000`, 4 MB).
  A board already flashed under the old table boots the same image
  unchanged: with `otadata` blank (erased `0xFF`), the bootloader's
  default is to boot `ota_0`, same behavior as booting `factory` today.
- `ota_1` (4 MB) lands in what was, before this change, ~9 MB of
  completely unallocated flash between the old `factory`'s end
  (`0x420000`) and `nvs_keys` (`0xD20000`) - the same unallocated region
  [0005](0005-esp-hosted-slave-ota.md) used for `slave_fw`. `nvs_keys`,
  `wifi_cfg`, and `slave_fw` keep their exact offsets.
- Net effect: **no `erase-flash` required** on an already-provisioned
  board, same reasoning as 0005 - the encrypted Wi-Fi credential store and
  co-processor firmware partition are untouched. ~5 MB of the original 9
  MB gap remains free.

### Versioning: GitHub release tag == `esp_app_desc_t.version`

A root `version.txt` (currently `0.10.0`) is wired via
`PROJECT_VER_FILE` in the top-level `CMakeLists.txt`, embedding its
content into the running image's `esp_app_desc_t.version`
(`esp_app_get_description()->version` at runtime). A GitHub release is
tagged with the same string (e.g. `0.10.0`). The OTA client compares the
release tag against the running version to decide whether an update is
available/newer - exact string equality is "no update needed"; this ADR
does not commit to a semver-aware ordering comparison since Phase 10's
own scope is a manual/notify-only trigger, not automatic "always take the
newest" logic. Bumping `version.txt` and cutting a matching GitHub release
tag together is a manual convention, not CI-automated.

### HTTP client sizing: GitHub's redirect `Location` header

GitHub's `/repos/{owner}/{repo}/releases/latest` API returns asset
`browser_download_url`s that redirect (302) to signed
`objects.githubusercontent.com` URLs with very long query strings. The
default `esp_http_client_config_t.buffer_size` (512 B) is too small to
hold that `Location` header and fails with a header-buffer overflow
before the redirect is even followed. Phase 10's OTA client sets
`buffer_size = 4096` (and `buffer_size_tx` at the same value) on both the
release-metadata request and the `esp_https_ota` config used for the
actual firmware download.

### No new secrets

Public repo, public release assets - no GitHub token/auth header, same
"no API keys required" property as `market_data_client`
([0002](0002-market-data-client.md)).

### Cutting a release: two assets, not one

`OTA_CLIENT_ASSET_NAME` (`crypto-market-data-ticker.bin`) is the raw app
image `idf.py build` produces — correct for an in-place OTA update (the
running board already has a valid bootloader/partition table, only the app
slot changes), but not enough by itself for a from-scratch flash of a blank
board, which also needs the bootloader and partition table.

To support the README's beginner flashing path without adding CI or code
changes, a release now carries a second, additional asset: a single merged
image combining bootloader + partition table + otadata + app, built from
the same `idf.py build` output. `@flash_args` uses paths relative to
`build/`, so run this from inside that directory (hardware-validated, see
`docs/validation/first-flash-esptool-hardware-test.md`):

```sh
cd build
esptool --chip esp32p4 merge-bin -o ../crypto-market-data-ticker-factory.bin @flash_args
cd ..
```

Upload both `build/crypto-market-data-ticker.bin` (unchanged, used by the
OTA client) and `crypto-market-data-ticker-factory.bin` (new, first-flash-only
asset) to the GitHub Release alongside the version tag. This stays a manual
step, same as the rest of this project's release convention above.

## Consequences

- `factory` is gone from `partitions.csv`; any *new* from-scratch flash
  (not an in-place upgrade) now flashes `ota_0` via `idf.py flash`, same
  command, different partition name.
- The bootloader now requires `CONFIG_APP_ROLLBACK_ENABLE` /
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (Kconfig) plus an explicit
  `esp_ota_mark_app_valid_cancel_rollback()` call somewhere in this
  project's own startup path to get anti-rollback boot-loop protection -
  deferred to Phase 10's rollback-safety delivery slice, not part of this
  partition-table change.
- Adds a second 4 MB app slot (`ota_1`) - flash budget for OTA moves from
  "0 spare" to "one full spare image", consistent with 0005's precedent of
  spending previously-unallocated flash on OTA infrastructure.

## Alternatives considered

- **`CONFIG_PARTITION_TABLE_TWO_OTA`** (ESP-IDF's built-in two-OTA-slot
  template): rejected - this project already has a custom table (encrypted
  `nvs_keys`/`wifi_cfg` from [0001](0001-wifi-connectivity.md), `slave_fw`
  from 0005) that the built-in template has no room for.
- **Semver-aware "always update to newest" comparison**: rejected for this
  phase - Phase 10's trigger is manual/notify-only (no silent auto-flash
  per the roadmap), so exact tag-match "is an update available" is
  sufficient; ordering logic can be added later without a partition-table
  or versioning-scheme change.
- **GitHub API token for higher rate limits**: rejected - the unauthenticated
  public rate limit (60 req/hour per IP) is far above what a periodic
  background check on a single device needs, and adding a token would
  introduce a secret this project otherwise has no need for.
