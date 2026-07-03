# Hardware validation: ESP-Hosted Wi-Fi link bring-up (Phase 5, PR 1)

## Environment

- Date: 2026-07-03
- Board: JC4880P443C_I_W (ESP32-P4 + ESP32-C6 module `JC-ESP32P4-M3`)
- Target: esp32p4
- ESP-IDF: v6.0.1
- Port: /dev/cu.usbmodem101
- Partition table changed in this test — board was `erase-flash`'d first
  (see `docs/decisions/0001-wifi-connectivity.md`)

## Command

```sh
idf.py -p /dev/cu.usbmodem101 erase-flash
idf.py -p /dev/cu.usbmodem101 flash monitor
```

## Expected result

- SDIO transport to the ESP32-C6 comes up and reports the co-processor
  firmware version.
- Memory budget check passes before `esp_wifi_init()`.
- `esp_wifi_start()` succeeds in STA mode.
- Display/touch (Phase 4) keep working unaffected.

## Observed logs

```text
I (1178) nvs_sec_provider: NVS Encryption - Registering HMAC-based scheme...
I (1185) H_SDIO_DRV: sdio_data_to_rx_buf_task started
...
I (1255) nvs: NVS partition "nvs" is encrypted.
I (1255) app_lifecycle: Application lifecycle started.
I (1285) st7701_mipi: LCD ID: FF FF FF
I (1425) board_jc4880p443c: ST7701 panel initialized: 480x800
I (1425) GT911: TouchPad_ID:0x39,0x31,0x31
I (1445) display_ui: Display UI started.
I (1445) wifi_manager: ESP-Hosted SDIO mempool budget: required=32592 internal_dma_largest=188416 spiram_dma_largest=0
I (1455) transport: Attempt connection with slave: retry[0]
W (1455) H_SDIO_DRV: Reset slave using GPIO[54]
I (2985) sdio_wrapper: SDIO master: Slot 1, Data-Lines: 4-bit Freq(KHz)[40000 KHz]
I (2985) sdio_wrapper: GPIOs: CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]
I (3125) H_SDIO_DRV: Card init success, TRANSPORT_RX_ACTIVE
I (3215) transport: Identified slave [esp32c6]
I (3225) transport: 	 * WLAN
I (3225) transport: 	   - HCI over SDIO
I (3235) transport: 	   - BLE only
W (3255) transport: Version mismatch: Host [2.9.0] < Co-proc [2.12.0] ==> Upgrade host to avoid compatibility issues
I (3275) transport: Base transport is set-up, TRANSPORT_TX_ACTIVE
I (4445) wifi_manager: Wi-Fi station started over ESP-Hosted link
I (4465) wifi_manager: ESP32-C6 co-processor firmware: 2.12.8
I (5455) rpc_req: Scan start Req
W (5535) wifi_manager: Startup scan failed: ESP_ERR_WIFI_STATE
I (8095) RPC_WRAP: ESP Event: Station mode: Connected
I (9115) esp_netif_handlers: sta ip: 192.168.100.238, mask: 255.255.255.0, gw: 192.168.100.1
```

## Result

Passed, with two real findings that change assumptions in
`docs/decisions/0001-wifi-connectivity.md` and inform the PR 3 design:

1. **Memory budget has comfortable headroom, not a tight fit.** Required
   32,592 bytes; largest free internal DMA block was 188,416 bytes — even
   with the display/LVGL layer (Phase 4) already running. The decision
   record's concern about display buffers starving the Hosted mempool did
   not materialize on this board/config; kept the check in place as a
   safety net regardless.
2. **The ESP32-C6 already had Wi-Fi association state from a previous test
   session (likely from `siswood-air-quality`, which shares this board),
   and reconnected on its own** — this project's code never called
   `esp_wifi_connect()` anywhere in this build, yet the board got an IP
   (`192.168.100.238`) a few seconds after `esp_wifi_start()`. Root cause:
   `idf.py erase-flash` only erases the **ESP32-P4's** flash; the ESP32-C6
   has its own separate flash that keeps whatever Wi-Fi state it was last
   in. This is a real trap for anyone reusing a board across projects —
   noting it here and in the decision record so the profile store work in
   PR 2/3 accounts for "C6 may already be connected to something we don't
   know about" as a possible boot state, not just "always starts
   disconnected."
3. **Scanning while the C6 is mid-(re)connect fails with
   `ESP_ERR_WIFI_STATE`.** This is expected ESP-IDF behavior (can't scan
   while associating) surfaced by finding #2. The PR 1 smoke-test scan is
   synchronous/best-effort and just logs the failure — the real policy
   state machine (PR 3) needs to treat `CONNECTING` as a state where scan
   requests are rejected or deferred, not silently retried forever.

The `Host [2.9.0] < Co-proc [2.12.0]` warning is the ESP-Hosted library's
own internal protocol-version log (distinct from the `==2.9.3` package
version pinned in `idf_component.yml`) — non-fatal, as designed; Wi-Fi
still started and connected successfully.

## Follow-up

- PR 2/3: profile store and policy must handle "already connected on boot"
  as a real startup state, not assume STA always starts idle.
- Consider whether a co-located `docs/hardware/jc4880p443c.md` note about
  the C6 keeping independent flash state (relevant for anyone re-flashing
  this exact physical board across projects) is worth adding.
