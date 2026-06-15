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
/* Panel: Seeed Studio reTerminal E1002 (7.3" Spectra E6, 800x480)   */
/* Pinout for Seeed Studio reTerminal E1002                          */
/* Based on: https://wiki.seeedstudio.com/reTerminal_E1002/          */
/* ------------------------------------------------------------------ */

/* E-paper panel SPI: single chip-select (как и у Waveshare 7.3") */
#define EPD_PIN_SCLK   7     /* GPIO7  - SPI Clock */
#define EPD_PIN_MOSI   9     /* GPIO9  - SPI Data (MOSI) */
#define EPD_PIN_CS     10    /* GPIO10 - Chip Select */
#define EPD_PIN_DC     11    /* GPIO11 - Data/Command */
#define EPD_PIN_RST    12    /* GPIO12 - Reset */
#define EPD_PIN_BUSY   13    /* GPIO13 - Busy pin */

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
/* reTerminal E1002 использует AXP2101 для управления питанием       */
/* I2C пины: SCL=GPIO20, SDA=GPIO19 (из документации Seeed)          */
/* ------------------------------------------------------------------ */
#define PMIC_I2C_PORT       0
#define PMIC_I2C_SCL        20    /* GPIO20 - I2C Clock */
#define PMIC_I2C_SDA        19    /* GPIO19 - I2C Data */
#define PMIC_I2C_HZ         100000
#define PMIC_AXP2101_ADDR   0x34
#define PMIC_PIN_IRQ        21    /* GPIO21 - PMIC IRQ */

/* ------------------------------------------------------------------ */
/* Buttons & LEDs (reTerminal E1002 специфичные пины)                 */
/* ------------------------------------------------------------------ */
/* reTerminal E1002 имеет несколько кнопок:
 *   POWER   - питание (отдельная схема)
 *   GREEN   - зеленая кнопка (GPIO3) - активный LOW
 *   RIGHT   - правая кнопка (GPIO4)  - активный LOW
 *   LEFT    - левая кнопка  (GPIO5)  - активный LOW
 */
#define BTN_PIN_GREEN   3     /* Зеленая кнопка, active low */
#define BTN_PIN_RIGHT   4     /* Правая кнопка, active low */
#define BTN_PIN_LEFT    5     /* Левая кнопка, active low */

#define BTN_TAP_MAX_MS    500    /* press shorter than this -> tap   */
#define BTN_HOLD_MIN_MS   2000   /* press at or beyond this -> hold  */

/* Status LED (на reTerminal E1002 есть встроенный светодиод) */
#define LED_PIN_STATUS   1     /* GPIO1 - встроенный светодиод */
#define LED_ON_LEVEL     1     /* Активный HIGH */
#define LED_OFF_LEVEL    0

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

/* Per-device topic namespace is tesserae/<device_id>/{frame/bin,config,status}. */
#ifndef MQTT_DEFAULT_DEVICE_ID
#define MQTT_DEFAULT_DEVICE_ID  "reterminal-e1002"
#endif
#ifndef MQTT_DEFAULT_USER
#define MQTT_DEFAULT_USER   ""
#endif
#ifndef MQTT_DEFAULT_PASS
#define MQTT_DEFAULT_PASS   ""
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID      "tesserae-reterminal-e1002-1"
#endif

/* The `kind` field in the heartbeat is just a UI hint for Tesserae's
 * Register form -- the renderer dispatches on panel_w/panel_h, both of
 * which we already publish below. */
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
#define NVS_KEY_LAST_HASH  "last_hash"   /* sha256 of last rendered URL */
#define NVS_KEY_SLEEP_S    "sleep_s"     /* user-configured deep-sleep duration */

/* Sanity bounds on sleep interval. The lower bound stops a publisher
 * accidentally turning the device into a 1-Hz spinner; the upper bound
 * is just "this is probably a bug". */
#define SLEEP_INTERVAL_MIN_S  30
#define SLEEP_INTERVAL_MAX_S  (7 * 24 * 60 * 60)
