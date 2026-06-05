/*
 * Waveshare 7.3" Spectra E6 (6-color) e-paper driver for the PhotoPainter.
 *
 * Ported from waveshareteam/ESP32-S3-PhotoPainter
 *   (01_Example/.../components/port_bsp/display_bsp.cpp -- EPD_Init,
 *    EPD_TurnOnDisplay, EPD_Display, command opcodes).
 * The init byte sequence is panel-specific and must stay byte-for-byte
 * exact -- don't "clean them up."
 *
 * Differs from the 13.3" panel driver in two ways:
 *   1. Single SPI chip-select (the 13.3" panel has two controllers).
 *   2. Panel power is gated by the AXP2101 (ALDOs), not a dedicated GPIO.
 *      epd_init() / epd_sleep() therefore call into pmic_rails_set().
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_config.h"

/* One-time SPI bus + GPIO setup. Safe to call multiple times. */
esp_err_t epd_port_init(void);

/* Full panel power-up + reset + init sequence. Call after epd_port_init()
 * and before epd_clear() / epd_display().
 *
 * NOTE: pmic_init() must have run first -- this function calls into
 * pmic_rails_set(true) to bring the panel rail up. */
void epd_init(void);

/* Fill the entire panel with a single palette color and refresh. */
void epd_clear(uint8_t color);

/* Push a full-frame buffer. `image` must point to EPD_BUF_BYTES bytes
 * (192000 = 800*480 / 2) laid out as 480 rows of 400 packed-nibble
 * bytes -- two pixels per byte, high nibble = even column. */
void epd_display(const uint8_t *image);

/* Paint the 6 panel colours as horizontal bands, top to bottom in palette
 * order (black, white, yellow, red, blue, green). The user-facing splash --
 * if every band shows the expected ink, the panel + driver + LUT are
 * all healthy. */
void epd_show_color_bars(void);

/* Diagnostic: paint all 8 possible nibble values (0x0..0x7) as 8 bands.
 * Useful when colours look wrong -- the output tells you the true
 * nibble->colour LUT of a specific panel batch. Not called in
 * production. */
void epd_show_palette_sweep(void);

/* Send DEEP_SLEEP command and drop the panel rail (via AXP2101).
 * After this, epd_init() must be called again before the next refresh. */
void epd_sleep(void);
