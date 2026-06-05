# tesserae-photopainter-7.3-bin-client

Battery-powered ESP32-S3 firmware that's the embedded client for the [Tesserae](https://github.com/dmellok/tesserae) server, ported to the [Waveshare ESP32-S3 PhotoPainter](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter) (7.3" 800×480 6-colour Spectra E6 e-paper panel on an ESP32-S3-WROOM-1-N16R8 module, AXP2101 PMIC, microSD slot).

The wake state machine, MQTT contract, captive-portal provisioning, and NVS schema are the same as the 13.3" client ([dmellok/tesserae-esp32-bin-client](https://github.com/dmellok/tesserae-esp32-bin-client)). What's different is the panel (single-CS, smaller, different init sequence), power management (AXP2101 over I2C instead of a GPIO panel-power gate and an ADC battery divider), and the heartbeat `kind` so the server can route the right frame format.

## Hardware mapping

| Component | Source firmware (13.3" E6) | This port (7.3" PhotoPainter) |
| --- | --- | --- |
| Module | ESP32-S3-WROOM-2-N32R16V | ESP32-S3-WROOM-1-N16R8 |
| Panel | 13.3" 1200×1600, dual-CS | 7.3" 800×480, single-CS |
| Frame size | 960 000 bytes | **192 000 bytes** |
| Panel power | GPIO1 (active-high) | AXP2101 ALDOs (via I²C) |
| Battery sense | ADC1 ch7 (GPIO8) + divider | AXP2101 fuel gauge (via I²C) |
| User button | RESET (double-tap) | BOOT (hold) + RESET (double-tap) |
| Status LED | none | Red GPIO45, Green GPIO42 |
| Extras | — | microSD slot, SHTC3, ES8311+ES7210 (all unused in v1) |

Pin map lives in [`include/app_config.h`](include/app_config.h); sourced from Waveshare's reference repo [waveshareteam/ESP32-S3-PhotoPainter](https://github.com/waveshareteam/ESP32-S3-PhotoPainter).

## MQTT contract

Same three topics as the 13.3" client under `tesserae/<device_id>/`:

| Topic | Direction | Retained | Purpose |
| --- | --- | --- | --- |
| `frame/bin` | server → device | yes | URL of the next `.bin` frame |
| `config` | server → device | yes | Runtime device settings |
| `status` | device → broker | yes | Wake-time heartbeat + LWT |

**Default `device_id` is `photopainter-73`** (not `esp32`) so the two panel kinds don't collide on a shared broker.

### Frame format

Raw, headerless, exactly **192 000 bytes** (`800 × 480 ÷ 2`), scanline order, two pixels per byte, high nibble = even column. Palette nibbles: `0x0`=Black, `0x1`=White, `0x2`=Yellow, `0x3`=Red, `0x5`=Blue, `0x6`=Green.

The heartbeat publishes `kind: "esp32_client"` and `panel_w: 800, panel_h: 480` — same `kind` as the 13.3" client. Tesserae's `esp32_bin` renderer dispatches on the panel dimensions, so no server-side change is required for the 7.3" panel.

## Build & flash

Requires [PlatformIO](https://platformio.org/). ESP-IDF 5.x and the Xtensa toolchain are pulled automatically on first build.

```bash
pio run                                              # build
pio run -e tesserae-photopainter-73-bin-client -t upload   # flash via USB-C
pio device monitor                                   # 115200 baud
```

For the dev shortcut, copy [`include/secrets.example.h`](include/secrets.example.h) to `include/secrets.h` and bake in WiFi/MQTT defaults. `secrets.h` is git-ignored.

## Provisioning

Two ways to enter the setup form (same form for both):

- **First boot / no creds.** SoftAP `Tesserae-Setup` (password `tesserae`) comes up; phone's captive-portal prompt opens the form.
- **Always-on editor.** Either **hold BOOT while pressing RESET** *or* **double-tap RESET within one wake window**. The device serves the form on its STA IP and advertises it over mDNS at `http://tesserae-<device_id>.local/`.

The double-tap path relies on the AXP2101 preserving RTC slow-memory across a PEK-triggered reset; if your unit clears it, the BOOT-hold path always works.

## Power notes

- **WiFi off before paint** — `wifi_sta_stop()` runs before `epd_init()` so the radio isn't holding ~80 mA during the ~25 s panel refresh. Same approach as the 13.3" client; biggest single battery saving in the render path.
- **Panel power via PMIC** — `pmic_rails_set(false)` is what gets the average current down before deep sleep, since the PhotoPainter has no dedicated GPIO panel-power gate. All four ALDO rails get dropped together (the Waveshare schematic doesn't publish the per-ALDO mapping; this mirrors their factory firmware behaviour).
- **Battery via I²C** — `pmic_battery_mv()` and `pmic_battery_pct()` come straight from the AXP2101 fuel gauge; no curve-fit ADC calibration, no Li-Po SoC table to maintain.

## Project layout

```
tesserae-photopainter-7.3-bin-client/
├── platformio.ini                # board, partitions, monitor, FW_VERSION
├── partitions.csv                # 14 MB factory app + NVS
├── sdkconfig.defaults            # PSRAM-octal, mbedTLS bundle, MQTT 3.1.1
├── include/
│   ├── app_config.h              # pinout + behaviour tunables
│   └── secrets.example.h         # template for credential overrides
└── src/
    ├── main.c                    # boot -> settings? -> splash -> connect -> render -> sleep
    ├── idf_component.yml         # managed deps (espressif/mdns)
    ├── epd_driver.{c,h}          # 7.3" Spectra E6 panel driver
    ├── pmic.{c,h}                # AXP2101 wrapper (battery + LDO rails)
    ├── heartbeat.{c,h}           # battery / RSSI / IP / panel size JSON
    ├── wifi_manager.{c,h}        # NVS-backed STA connect
    ├── provisioning.{c,h}        # captive portal + always-on settings server
    ├── mqtt_config.{c,h}         # NVS-backed broker URI / device_id
    ├── mqtt_handler.{c,h}        # single-shot subscribe + dispatch
    ├── image_fetcher.{c,h}       # HTTP download into PSRAM
    └── image_decoder.{c,h}       # strict 192000-byte panel-native validation
```

No tests -- smoke-test on real hardware. Recommended validation after any change to the wake state machine:

1. Flash a fresh board, walk it through the captive portal.
2. `mosquitto_pub -t tesserae/photopainter-73/frame/bin -r -m '{"url":"http://.../test.bin"}'` and confirm the panel paints.
3. Hold BOOT, press RESET, confirm `http://tesserae-photopainter-73.local/` (or the device IP) serves the settings form pre-filled with live values.

## Credits

Panel driver in [`src/epd_driver.c`](src/epd_driver.c) is a port of Waveshare's reference at [`01_Example/.../components/port_bsp/display_bsp.cpp`](https://github.com/waveshareteam/ESP32-S3-PhotoPainter/blob/main/01_Example/xiaozhi-esp32/components/port_bsp/display_bsp.cpp). The init byte sequence is panel-specific and kept byte-for-byte exact. PMIC register addresses come from Waveshare's bundled XPowersLib component in the same repo.

Wake state machine, captive-portal provisioning, NVS schema, and MQTT contract are forked verbatim from [`dmellok/tesserae-esp32-bin-client`](https://github.com/dmellok/tesserae-esp32-bin-client).

## License

MIT.
