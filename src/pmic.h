/*
 * AXP2101 PMIC wrapper for the Waveshare ESP32-S3 PhotoPainter.
 *
 * Replaces the GPIO panel-power gate + ADC battery divider used on the
 * 13.3" board. On the PhotoPainter:
 *   - Panel power, SD power, codec power are all behind AXP2101 LDO rails.
 *     The official Waveshare reference enables ALDO1..4 together at 3.3V
 *     during normal operation and disables all four before sleep; we
 *     mirror that because the schematic doesn't publish the per-LDO map.
 *   - Battery voltage and percentage come from the AXP2101 fuel gauge
 *     over I2C -- no ADC divider, no manual Li-Po curve.
 *
 * This wrapper is intentionally minimal: enough to power the analog
 * rails up before render, read battery for the heartbeat, and power
 * everything down for deep sleep. Anything more (charger config,
 * interrupts, BATFET control) we'll add when we actually need it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Bring the AXP2101 up over I2C and put it in a known state:
 *   - VBUS current limit 2 A (matches Waveshare reference)
 *   - ALDO1..4 set to 3.3 V and enabled (powers panel / SD / codec rails)
 *   - charge current 500 mA, termination 25 mA
 *
 * Safe to call once per cold boot and after every deep-sleep wake.
 * Returns ESP_OK on success; ESP_FAIL if the chip didn't ACK on the
 * configured I2C bus (treat as "hardware MIA" -- log and keep going). */
esp_err_t pmic_init(void);

/* Toggle the analog-rail LDOs. ALDO1..4 are the rails the official
 * Waveshare firmware turns off before deep sleep, so calling
 * pmic_rails_set(false) before esp_deep_sleep_start() is what gets you
 * the ~3 uA sleep floor. */
esp_err_t pmic_rails_set(bool enabled);

/* Battery telemetry from the on-chip fuel gauge. Returns:
 *   - battery_mv:  millivolts at the cell, or 0 if no battery / read fails
 *   - battery_pct: 0..100, or -1 if no battery / read fails
 * Heartbeat publishes both raw values; Tesserae handles the "unknown"
 * marker semantics on the server side. */
uint16_t pmic_battery_mv(void);
int      pmic_battery_pct(void);

/* True iff a battery is detected on the AXP2101 BATSENSE pin.
 * Use to gate sleep-vs-restart-loop decisions when running on bench
 * power (no battery installed). */
bool pmic_battery_present(void);
