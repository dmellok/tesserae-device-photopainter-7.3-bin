/*
 * Boot splashes painted onto the panel. Both blobs are produced offline by
 * tools/gen_splash.py (PNG -> Floyd-Steinberg dither -> 4-bpp packed), then
 * embedded via CMake's EMBED_FILES so the firmware streams them straight
 * to epd_display() with no decode at runtime.
 *
 * Each call brings the panel up, refreshes (~30 s), and puts it back to
 * sleep -- safe to invoke either with the panel still off or already
 * initialised (epd_port_init is idempotent).
 */
#pragma once

#include "esp_err.h"

/* Logo centered on white. Shown on cold-boot when WiFi creds are present
 * so the user knows the device just booted (vs. has been asleep). */
esp_err_t splash_show_logo(void);

/* Logo on the left, baked WPA-format QR for the captive AP on the right.
 * Shown right before the captive portal comes up so the user can join
 * Tesserae-Setup by scanning instead of typing the SSID/password. The QR
 * encodes the SAME credentials advertised by the SoftAP -- if those ever
 * change in app_config.h, rerun tools/gen_splash.py to regenerate the
 * blob. */
esp_err_t splash_show_portal(void);

/* Logo centered with "Connected to Tesserae" / "Waiting for first frame"
 * labels. Painted exactly ONCE, on the first boot after the user submits
 * the captive portal -- replaces the portal-splash QR on the panel so
 * the visible state matches "we're past pairing and waiting on the
 * server." Triggered by an NVS flag set in provisioning.c's h_save and
 * cleared in main.c after the splash paints. */
esp_err_t splash_show_paired(void);
