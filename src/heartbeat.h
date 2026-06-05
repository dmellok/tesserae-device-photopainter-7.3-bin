/*
 * Wake-time heartbeat: battery, signal strength, IP.
 *
 * Builds a compact JSON object suitable for publishing to a retained
 * MQTT topic so a companion dashboard can see "last known device state"
 * even while the device is asleep.
 */
#pragma once

#include <stddef.h>

#include "esp_system.h"   /* esp_reset_reason_t */

/* Fill `dst` with a JSON document like:
 *   {"battery_mv":3950,"battery_pct":67,"rssi":-45,
 *    "ip":"192.168.50.234","fw_version":"0.4.0",
 *    "kind":"esp32_client","panel_w":1200,"panel_h":1600,
 *    "sleep_interval_s":900,"wake_reason":"timer"}
 *
 * `sleep_interval_s` is the duration the device intends to deep-sleep for
 * after this wake -- lets the server compute heartbeat-age vs. expected
 * cadence (instead of treating each sleep as a fault). `wake_reason` is a
 * short string mapping of the esp_reset_reason_t for this wake.
 *
 * Never fails -- unknown fields are emitted as 0 / empty string. The
 * caller's buffer should be at least 256 bytes; smaller is safe but
 * fields may be truncated. */
void heartbeat_format_json(char *dst, size_t dst_sz,
                           int sleep_interval_s,
                           esp_reset_reason_t reset_reason);
