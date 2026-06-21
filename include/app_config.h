/*
 * Project-wide tunables. Edit here, not scattered across .c files.
 *
 * For local credential overrides (dev shortcut to bypass the captive
 * portal), copy include/secrets.example.h to include/secrets.h and fill
 * in the WIFI_DEFAULT_* / MQTT_DEFAULT_* macros there. secrets.h is
 * git-ignored.
 */
#pragma once

#include <stdint.h>

/* Pull in user-local overrides if they exist. Falls through silently if
 * secrets.h hasn't been created -- the build doesn't depend on it. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/* ------------------------------------------------------------------ */
/* Panel: Waveshare ESP32-S3 PhotoPainter (7.3" Spectra E6, 800x480)  */
/* Pinout taken from Waveshare's reference code:                      */
/*   waveshareteam/ESP32-S3-PhotoPainter                              */
/*     04_PowerConsumptionTest/.../bsp_config.h     (EPD/SDMMC/LED)   */
/*     05_ArduinoExample/01_Audio_Test.ino           (I2C)            */
/*     01_Example/.../components/port_bsp/button_bsp.c (PWR_KEY)      */
/* ------------------------------------------------------------------ */

/* E-paper panel SPI: single chip-select (unlike the 13.3" dual-CS panel) */
#define EPD_PIN_SCLK   10
#define EPD_PIN_MOSI   11
#define EPD_PIN_CS     9
#define EPD_PIN_DC     8
#define EPD_PIN_RST    12
#define EPD_PIN_BUSY   13

#define EPD_SPI_HOST   SPI3_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Native orientation is landscape on this panel. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 192000 */

/* 6-color palette indices (nibble values). 4 and 7 are reserved. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* ------------------------------------------------------------------ */
/* Power management: AXP2101 PMIC over I2C                            */
/* The PhotoPainter has no GPIO panel-power gate and no ADC battery   */
/* divider; the AXP2101 handles both. Battery mV/% come from the      */
/* on-chip fuel gauge; panel rail is an AXP2101 LDO output.           */
/* ------------------------------------------------------------------ */
#define PMIC_I2C_PORT       0
#define PMIC_I2C_SCL        48
#define PMIC_I2C_SDA        47
/* Waveshare's reference talks to the AXP2101 at 100 kHz; bumping to
 * 400 kHz returns ESP_ERR_INVALID_STATE on every transaction on this
 * board (probably weak pull-ups on the long PMIC trace). */
#define PMIC_I2C_HZ         100000
#define PMIC_AXP2101_ADDR   0x34
#define PMIC_PIN_IRQ        21

/* ------------------------------------------------------------------ */
/* Buttons & LEDs                                                     */
/* ------------------------------------------------------------------ */
/* The PhotoPainter exposes two user-facing buttons:
 *   BOOT (GPIO0)  -- ESP32-S3 boot-strap pin; ACTIVE LOW
 *   PWR  (GPIO5)  -- AXP2101 PEK output; ACTIVE HIGH (inverted via PMIC)
 *
 * BOOT is the boot-strap pin, so holding it LOW at reset puts the chip
 * into ROM serial-download mode and our firmware never runs. We can
 * only sample it *after* boot completes -- never at reset. PWR is owned
 * by the AXP2101: short-press wakes the SoC, long-press kills it, all
 * in hardware. The firmware reads BOOT during the wake window for two
 * user actions:
 *
 *   tap   (<BTN_TAP_MAX_MS)   -> force refresh (clear the URL hash)
 *   hold  (>=BTN_HOLD_MIN_MS) -> enter the LAN settings editor
 *
 * GPIO4 is a third button ("GP4") in Waveshare's reference, but its
 * presence on the production PhotoPainter casing isn't documented, so
 * we don't map it. */
#define BTN_PIN_PWR    5     /* AXP2101 PEK, active high */
#define BTN_PIN_BOOT   0     /* "BOOT" labeled button, active low */

#define BTN_TAP_MAX_MS    500    /* press shorter than this -> tap   */
#define BTN_HOLD_MIN_MS   2000   /* press at or beyond this -> hold  */

/* Status LEDs. Both active-LOW (LED_ON == 0). */
#define LED_PIN_RED    45
#define LED_PIN_GREEN  42
#define LED_ON_LEVEL   0
#define LED_OFF_LEVEL  1

/* ------------------------------------------------------------------ */
/* Application behavior                                               */
/* ------------------------------------------------------------------ */

/* Reported in the status heartbeat so Tesserae can show which firmware
 * each device is running. The authoritative value is set in platformio.ini
 * (build_flags = -DFW_VERSION=\"x.y.z\"); this is just a fallback so the
 * file still compiles outside PlatformIO. */
#ifndef FW_VERSION
#define FW_VERSION         "0.0.0-dev"
#endif

/* How long to deep-sleep between MQTT checks. 5 minutes is short
 * enough to feel responsive while still keeping the wake-cycle cost
 * well under the panel-refresh cost on a multi-color update. */
#define SLEEP_INTERVAL_S   (5 * 60)

/* Cap on how long we'll wait for a retained MQTT message after
 * subscribing, before giving up and going back to sleep. */
#define MQTT_WAIT_MS       8000

/* WiFi STA connect attempt: retry count and per-attempt timeout. */
#define WIFI_CONNECT_RETRIES   5
#define WIFI_CONNECT_TIMEOUT_MS 15000

/* How long the provisioning portal stays up after a failed STA
 * connect. Power gets burned the whole time, so don't make it huge. */
#define PROVISION_PORTAL_TIMEOUT_S  (10 * 60)

/* SoftAP credentials shown to the user during provisioning. */
#define PROVISION_AP_SSID    "Tesserae-Setup"
#define PROVISION_AP_PASS    "tesserae"     /* >= 8 chars or use open AP */

/* ------------------------------------------------------------------ */
/* WiFi / MQTT compile-time defaults                                  */
/* ------------------------------------------------------------------ */
/* Precedence on each wake:
 *     NVS (set via portal)  >  these defaults  >  empty (portal triggers)
 *
 * secrets.h may override any of these; otherwise WiFi defaults to empty
 * (no auto-connect) and MQTT defaults to placeholders that will fail
 * gracefully if the user hasn't run the portal yet. */

#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID   ""
#endif
#ifndef WIFI_DEFAULT_PASS
#define WIFI_DEFAULT_PASS   ""
#endif

#ifndef MQTT_DEFAULT_URI
#define MQTT_DEFAULT_URI    "mqtt://homeassistant.local:1883"
#endif
/* Per-device topic namespace is tesserae/<device_id>/{frame/bin,config,status}.
 * Default differs from the 13.3" client ("esp32") so the two panels can
 * share one broker without colliding. */
#ifndef MQTT_DEFAULT_DEVICE_ID
#define MQTT_DEFAULT_DEVICE_ID  "photopainter-73"
#endif
#ifndef MQTT_DEFAULT_USER
#define MQTT_DEFAULT_USER   ""
#endif
#ifndef MQTT_DEFAULT_PASS
#define MQTT_DEFAULT_PASS   ""
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID      "tesserae-photopainter-73-1"
#endif

/* The `kind` field in the heartbeat is just a UI hint for Tesserae's
 * Register form -- the renderer dispatches on panel_w/panel_h, both of
 * which we already publish below. Reusing the 13.3" client's value
 * means the server needs no new renderer for the 7.3" panel. */
#ifndef DEVICE_KIND
#define DEVICE_KIND         "esp32_client"
#endif

/* Dev shortcut: define DEV_DISABLE_SLEEP (in secrets.h) to swap the
 * 15-min deep sleep for a short delay + software restart loop. Useful
 * while iterating with the serial monitor open. Cold-boot splash only
 * fires on power-on / RESET button, not on the software restart, so
 * each iteration is fast. */
#ifndef DEV_LOOP_INTERVAL_S
#define DEV_LOOP_INTERVAL_S 10
#endif

/* NVS namespaces / keys */
#define NVS_NS_WIFI        "wifi"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "pass"

#define NVS_NS_MQTT          "mqtt"
#define NVS_KEY_MQTT_URI     "uri"
#define NVS_KEY_MQTT_TOPIC   "topic"      /* legacy; read once at migration, then erased */
#define NVS_KEY_MQTT_DEVICE_ID "device_id"
#define NVS_KEY_MQTT_USER    "user"
#define NVS_KEY_MQTT_PASS    "pass"

/* device_id charset/length: 2-32 chars, [a-z0-9_-], must start with a letter.
 * Keep in sync with Tesserae's device.json validation. */
#define DEVICE_ID_MIN_LEN   2
#define DEVICE_ID_MAX_LEN   32

#define NVS_NS_STATE       "state"
#define NVS_KEY_LAST_HASH  "last_hash"   /* sha256 of last rendered URL (mqtt mode) */
#define NVS_KEY_SLEEP_S    "sleep_s"     /* user-configured deep-sleep duration */
#define NVS_KEY_PAIRED_PEND "paired_pen" /* u8: 1 == captive portal saved creds, paint
                                          *      "Connected/Waiting for first frame"
                                          *      splash on next boot once WiFi is up */

/* Sanity bounds on sleep interval. The lower bound stops a publisher
 * accidentally turning the device into a 1-Hz spinner; the upper bound
 * is just "this is probably a bug". */
#define SLEEP_INTERVAL_MIN_S  30
#define SLEEP_INTERVAL_MAX_S  (7 * 24 * 60 * 60)

/* ------------------------------------------------------------------ */
/* Transport mode + REST-API configuration                            */
/* ------------------------------------------------------------------ */
/* The firmware can talk to Tesserae two ways:
 *   0 = mqtt  : retained-topic subscribe + publish (the original)
 *   1 = rest  : HTTP polling against /api/v1/device/<id>/{frame,status}
 *               (no broker required; default for new installs)
 * Existing installs upgrading from <0.3.0 see transport_mode absent in
 * NVS and default to MQTT for backward compatibility. New first-boots
 * pick from the captive portal radio. */
typedef enum {
    TRANSPORT_MODE_MQTT = 0,
    TRANSPORT_MODE_REST = 1,
} transport_mode_t;

#define NVS_NS_REST            "rest"
#define NVS_KEY_TRANSPORT_MODE "transport"     /* u8: transport_mode_t */
#define NVS_KEY_SERVER_URL     "server_url"    /* e.g. http://tesserae.local:8765 */
#define NVS_KEY_DEVICE_TOKEN   "device_token"  /* bearer; set after register/discover */
#define NVS_KEY_PAIRING_CODE   "pair_code"     /* 6-char; cleared after register */
#define NVS_KEY_FRAME_ETAG     "frame_etag"    /* for If-None-Match -> 304 path */

/* Discover-retry default when the server says "registered: false" with
 * no retry_after_s hint. The admin clicks Register on the Tesserae UI;
 * the next discover after this window claims the token. 30 s gives the
 * admin time without blowing the battery on a constant retry storm. */
#define REST_DISCOVER_RETRY_DEFAULT_S   30

/* Fallback when /status response omits next_poll_s. Same value as the
 * MQTT path's SLEEP_INTERVAL_S keeps the failure mode predictable. */
#define REST_NEXT_POLL_FALLBACK_S       SLEEP_INTERVAL_S

/* HTTP client tuning (esp_http_client). Buffer at 1024 because the
 * /frame response headers (ETag + standard set) overflow the 512-byte
 * default. 15 s for JSON endpoints; the .bin fetch path uses its own
 * longer timeout via image_fetcher. */
#define REST_HTTP_BUFFER_SIZE     1024
#define REST_HTTP_TIMEOUT_MS      15000

/* Compile-time REST defaults. Optional -- secrets.h may override either
 * to skip captive-portal entry on a freshly flashed dev board. */
#ifndef REST_DEFAULT_SERVER_URL
#define REST_DEFAULT_SERVER_URL   ""
#endif
#ifndef REST_DEFAULT_PAIRING_CODE
#define REST_DEFAULT_PAIRING_CODE ""
#endif
#ifndef REST_DEFAULT_DEVICE_TOKEN
#define REST_DEFAULT_DEVICE_TOKEN ""
#endif

/* Default transport when NVS has no transport_mode key set. Existing
 * MQTT installs upgrading from <0.3.0 land here -- explicit
 * backward-compat. A secrets.h with REST_DEFAULT_SERVER_URL filled in
 * overrides this at runtime in load_transport_mode() (see main.c) so
 * developers can flash a clean board straight into REST mode. */
#ifndef TRANSPORT_DEFAULT_MODE
#define TRANSPORT_DEFAULT_MODE    TRANSPORT_MODE_MQTT
#endif
