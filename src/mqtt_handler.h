/*
 * Single-shot MQTT job grabber.
 *
 * Each wake-up:
 *   1. connects to the broker
 *   2. subscribes to the update topic
 *   3. waits MQTT_WAIT_MS for the retained message
 *   4. returns the URL string (and any options) it carries
 *   5. tears down
 *
 * The Python listener publishes its job either as a bare URL string or
 * as a JSON object with a "url" field; we accept both, since either is
 * a one-line change on the publisher side.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char url[256];     /* zero-terminated; empty if no message arrived */
} mqtt_job_t;

/* Blocks for up to MQTT_WAIT_MS. Returns ESP_OK if a job was captured
 * (job->url is non-empty), ESP_ERR_TIMEOUT if the broker had no retained
 * message, or another esp_err_t on transport failure.
 *
 * If `heartbeat_json` is non-NULL and non-empty, that string is published
 * retained to tesserae/<device_id>/status right after we connect, before
 * waiting for the update message -- so a dying device still reports its
 * state even if the render path later fails. */
esp_err_t mqtt_fetch_retained(mqtt_job_t *job, const char *heartbeat_json);
