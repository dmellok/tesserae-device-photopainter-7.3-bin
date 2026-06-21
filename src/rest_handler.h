/*
 * Tesserae REST transport (transport_mode = TRANSPORT_MODE_REST).
 *
 * One-shot wake loop. Caller does NOT need to be authenticated
 * up-front: the loop will discover / register on its own if NVS has no
 * device_token. Token is persisted to NVS the first time it's
 * obtained, so subsequent wakes go straight to the frame GET.
 *
 * Flow on each wake (any failure deep-sleeps the device for the
 * fallback or server-suggested interval; nothing in here is fatal to
 * the firmware):
 *
 *   1. ensure_device_token()
 *        - have it in NVS?           -> return
 *        - pairing_code present?      -> POST /device/register with
 *                                        X-Pairing-Code header
 *        - otherwise                  -> POST /device/discover and let
 *                                        the admin claim by MAC
 *   2. GET /device/<id>/frame  (If-None-Match: <last_etag>)
 *        - 200 -> store new ETag, fetch the URL body, paint
 *        - 304 -> skip fetch + paint (panel still current)
 *        - 204 -> skip (server hasn't rendered anything yet)
 *   3. POST /device/<id>/status  (heartbeat JSON, same shape as MQTT)
 *        - apply config.sleep_interval_s to NVS
 *        - read next_poll_s for the deep-sleep duration
 *   4. esp_deep_sleep_start() for next_poll_s seconds
 *
 * Returns the number of seconds the caller should deep-sleep for
 * after this wake. 0 means "use the firmware's configured default".
 */
#pragma once

#include "esp_err.h"
#include "esp_system.h"   /* esp_reset_reason_t */

/* Run one wake cycle of the REST transport. WiFi must already be up.
 *
 * On success returns the deep-sleep duration the server suggested
 * (via /status response's next_poll_s) clamped to
 * [SLEEP_INTERVAL_MIN_S, SLEEP_INTERVAL_MAX_S]. On any failure that's
 * not fatal to the firmware (network glitch, server down, 401, 403),
 * returns the firmware's configured fallback so the device keeps
 * cycling instead of getting stuck.
 *
 * `reset_reason` is forwarded into the status payload for the same
 * server-side diagnostics the MQTT path provides. */
int rest_run_loop(esp_reset_reason_t reset_reason);
