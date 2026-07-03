# Hardware validation: Wi-Fi autoconnect (Phase 5, PR 3)

## Environment

- Date: 2026-07-03
- Board: JC4880P443C_I_W
- Target: esp32p4
- ESP-IDF: v6.0.1
- Port: /dev/cu.usbmodem101

## Command

```sh
idf.py erase-flash
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Dev credentials were set locally via `menuconfig` (`CONFIG_WIFI_MANAGER_DEV_SSID`
/ `CONFIG_WIFI_MANAGER_DEV_PASSWORD`), never committed - see
`components/wifi_manager/Kconfig`.

## Scenario 1: boot with dev-injected profile, autoconnect

### Expected result

- No saved profiles exist yet (fresh/erased `wifi_cfg` partition).
- The Kconfig-configured dev SSID/password is injected as a profile on
  first boot.
- Policy autoconnects to it (origin = AUTOCONNECT) without any user command.
- Connection succeeds, an IP address is obtained.

### Observed logs

```text
I (4489) wifi_profile_store: Wi-Fi profile store ready (encrypted)
I (4489) wifi_manager: Wi-Fi profile storage available: yes
I (5549) RPC_WRAP: ESP Event: Station mode: Disconnected
I (5579) wifi_manager: Connecting to 'TV' (origin=1)
I (5919) RPC_WRAP: ESP Event: Station mode: Connected
I (7579) esp_netif_handlers: sta ip: 192.168.100.238, mask: 255.255.255.0, gw: 192.168.100.1
```

`origin=1` is `WIFI_POLICY_ORIGIN_AUTOCONNECT` - confirms the policy state
machine (not a manual command) drove this connection, exactly as designed
for a first-boot dev-cred bring-up.

### Result

**Passed.**

## Scenario 2: auth failure / profile blocking

### Expected result

Deliberately wrong password on the dev profile should produce
`WIFI_EVENT_STA_DISCONNECTED` with an auth-class reason, mapped by
`wifi_policy_classify_reason()` to `WIFI_POLICY_FAIL_AUTH`, and after
`auth_block_threshold` (2) consecutive auth failures, the profile should be
blocked (`WIFI_MANAGER_EVENT_AUTH_FAILED`, `ACT_SET_BLOCKED`), then (being
the only profile) `WIFI_MANAGER_EVENT_ALL_PROFILES_BLOCKED`.

### Result

**Inconclusive on hardware, not blocking.** Setting `CONFIG_WIFI_MANAGER_DEV_PASSWORD`
to a deliberately wrong value and reflashing still resulted in a successful
connection to the same network. Two possible explanations, neither
confirmed: the test network may not enforce the PSK as strictly as assumed
at the association layer used here, or state from a prior test run
leaked through. This scenario **is** covered by the pure-logic host tests
(`test_wifi_policy_authblock.c`, 14/14 checks), which exercise the
blocking/unblocking state machine directly and deterministically - just not
end-to-end against a real access point in this session. Flagged as a
follow-up to retest with a network known to enforce WPA2-PSK strictly.

## Known pitfall discovered during testing

**Do not use `esptool erase_region` on just the `wifi_cfg` partition to
reset saved profiles for testing.** Doing so twice in a row (to swap
between a correct and a deliberately wrong dev password) left the board in
a state where `esp_wifi_connect()` failed immediately with
`ESP_ERR_WIFI_SSID` and an empty SSID in the log, even though the profile
store reported itself available and the compiled-in Kconfig value was
confirmed correct in `build/config/sdkconfig.h`. Root cause not fully
isolated - suspected inconsistency between the encrypted NVS partition
state and the `nvs_keys` partition (which was intentionally left
untouched) after repeated targeted erases. A full `idf.py erase-flash`
immediately resolved it and reproduced Scenario 1 cleanly. Documented here
so a future targeted-erase workflow either erases `nvs_keys` +
`wifi_cfg` together, or just uses full `erase-flash` for encrypted-NVS
testing.

## Follow-up

- Retest auth-failure/blocking end-to-end against a network confirmed to
  strictly enforce WPA2-PSK.
- Reconnect-after-router-loss and continuous-cycle-restart scenarios were
  not exercised on hardware in this session (require physically
  power-cycling a router); covered by `test_wifi_policy_cycle.c` and
  `test_wifi_policy_retry.c` at the logic level.
- Fallback-to-previous-network (`wifi_manager_connect_new` while already
  connected) was not exercised on hardware in this session; covered by
  `test_wifi_policy_fallback.c` at the logic level.
