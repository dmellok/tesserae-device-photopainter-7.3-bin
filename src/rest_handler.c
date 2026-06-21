/*
 * REST transport for Tesserae. See rest_handler.h for the contract.
 *
 * Three external dependencies, all already used by the MQTT path:
 *   - esp_http_client : the HTTP client (also used by image_fetcher)
 *   - cJSON           : response-body parser (also used by esp-mqtt)
 *   - nvs_flash       : same handle layout the rest of the firmware uses
 *
 * The wake loop is single-shot: one register/discover (if no token),
 * one frame GET, one frame-body fetch + paint (only on 200 with a new
 * URL), one status POST, then return the server's suggested next_poll_s
 * to the caller. No background tasks, no event groups, no retries
 * beyond what's required to keep the device from getting stuck.
 */

#include "rest_handler.h"
#include "app_config.h"
#include "epd_driver.h"
#include "heartbeat.h"
#include "image_decoder.h"
#include "image_fetcher.h"
#include "mqtt_config.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "rest";

/* ====================================================================
 *  NVS helpers
 * ==================================================================== */

static esp_err_t nvs_get_str_buf(const char *ns, const char *key,
                                 char *out, size_t out_sz) {
    if (!out || out_sz == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_set_str_buf(const char *ns, const char *key,
                                 const char *value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void nvs_erase_key_quiet(const char *ns, const char *key) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

/* ====================================================================
 *  Wall-clock seeding from /status server_time field
 * ==================================================================== */

/* Server returns a unix-seconds float (e.g. 1781941884.34). Use it to
 * seed the RTC when SNTP failed -- handy for the PhotoPainter's flaky
 * RTC-across-deep-sleep behaviour (see ensure_time_synced in main.c). */
static void apply_server_time(double server_time) {
    if (server_time < 1700000000.0 || server_time > 2200000000.0) {
        return;   /* outside the sane-epoch window; ignore */
    }
    struct timeval tv = {
        .tv_sec  = (time_t) server_time,
        .tv_usec = (suseconds_t)((server_time - (double)(time_t) server_time) * 1e6),
    };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "set wall clock from server_time=%.3f", server_time);
}

/* ====================================================================
 *  URL + identity helpers
 * ==================================================================== */

/* The server URL in NVS / secrets.h has the form
 *     http://host:port
 * (no trailing slash, no /api/v1). Paths get composed here. */
static esp_err_t build_url(char *out, size_t out_sz,
                           const char *server_url, const char *path_fmt, ...)
                           __attribute__((format(printf, 4, 5)));
static esp_err_t build_url(char *out, size_t out_sz,
                           const char *server_url, const char *path_fmt, ...) {
    if (!out || out_sz == 0 || !server_url || !*server_url) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t base_len = strlen(server_url);
    /* Strip trailing slash for clean concatenation. */
    while (base_len > 0 && server_url[base_len - 1] == '/') base_len--;
    if (base_len + 32 > out_sz) return ESP_ERR_INVALID_SIZE;
    memcpy(out, server_url, base_len);
    out[base_len] = '\0';

    char path[256];
    va_list ap;
    va_start(ap, path_fmt);
    int n = vsnprintf(path, sizeof(path), path_fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    if (base_len + (size_t) n + 1 > out_sz) return ESP_ERR_INVALID_SIZE;
    memcpy(out + base_len, path, (size_t) n + 1);
    return ESP_OK;
}

/* WiFi STA MAC as lowercase hex, no separators. Matches what the
 * Tesserae server stores when admin clicks Register on a discovered
 * device -- discover-then-claim looks up by exact-match on this. */
static esp_err_t get_mac_hex(char *out, size_t out_sz) {
    if (out_sz < 13) return ESP_ERR_INVALID_SIZE;
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) return err;
    snprintf(out, out_sz, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/* ====================================================================
 *  HTTP client: response body capture
 * ==================================================================== */

/* Slurp the response body into a heap buffer. Used for the JSON
 * endpoints (capped at REST_HTTP_BUFFER_SIZE × 8 = 8 KB which is far
 * more than any expected status/discover/frame response). */
#define REST_RESPONSE_CAP   (REST_HTTP_BUFFER_SIZE * 8)

typedef struct {
    char  *buf;       /* heap; caller must free */
    size_t cap;
    size_t len;
    char   etag[96];  /* captured from response if present */
} body_capture_t;

static esp_err_t on_http_event(esp_http_client_event_t *e) {
    body_capture_t *cap = e->user_data;
    if (!cap) return ESP_OK;

    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(e->header_key, "ETag") == 0) {
            strncpy(cap->etag, e->header_value, sizeof(cap->etag) - 1);
            cap->etag[sizeof(cap->etag) - 1] = '\0';
        }
        break;
    case HTTP_EVENT_ON_DATA: {
        if (!cap->buf) {
            cap->cap = REST_RESPONSE_CAP;
            cap->buf = malloc(cap->cap);
            if (!cap->buf) {
                cap->cap = 0;
                return ESP_FAIL;
            }
            cap->len = 0;
        }
        if (cap->len + e->data_len + 1 > cap->cap) {
            /* Body would overflow; truncate. The JSON parser will fail
             * downstream, which is the right behaviour. */
            return ESP_OK;
        }
        memcpy(cap->buf + cap->len, e->data, e->data_len);
        cap->len += e->data_len;
        cap->buf[cap->len] = '\0';
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

static void body_capture_free(body_capture_t *cap) {
    if (cap && cap->buf) {
        free(cap->buf);
        cap->buf = NULL;
        cap->cap = 0;
        cap->len = 0;
    }
}

/* Single-shot HTTP request. The `bearer_token` and `pairing_code` args
 * may be NULL/empty to skip the respective header. `body_json` is the
 * POST body (NULL/empty for GET); `if_none_match` becomes the
 * `If-None-Match` header on GETs. */
static esp_err_t http_request(esp_http_client_method_t method,
                              const char *url,
                              const char *bearer_token,
                              const char *pairing_code,
                              const char *if_none_match,
                              const char *body_json,
                              int *out_status,
                              body_capture_t *out_body) {
    *out_status = 0;

    esp_http_client_config_t cfg = {
        .url             = url,
        .method          = method,
        .timeout_ms      = REST_HTTP_TIMEOUT_MS,
        .buffer_size     = REST_HTTP_BUFFER_SIZE,
        .buffer_size_tx  = REST_HTTP_BUFFER_SIZE,
        .event_handler   = on_http_event,
        .user_data       = out_body,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    char auth_buf[160];
    if (bearer_token && *bearer_token) {
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", bearer_token);
        esp_http_client_set_header(cli, "Authorization", auth_buf);
        /* Fallback header for HTTP-client variants that strip the
         * standard Authorization header (rare on stock IDF, but the
         * server accepts X-Tesserae-Token as a documented alternative). */
        esp_http_client_set_header(cli, "X-Tesserae-Token", bearer_token);
    }
    if (pairing_code && *pairing_code) {
        esp_http_client_set_header(cli, "X-Pairing-Code", pairing_code);
    }
    if (if_none_match && *if_none_match) {
        esp_http_client_set_header(cli, "If-None-Match", if_none_match);
    }
    if (body_json && *body_json) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_post_field(cli, body_json, strlen(body_json));
    }

    esp_err_t err = esp_http_client_perform(cli);
    if (err == ESP_OK) {
        *out_status = esp_http_client_get_status_code(cli);
    } else {
        ESP_LOGW(TAG, "%s %s failed: %s",
                 method == HTTP_METHOD_GET ? "GET" : "POST",
                 url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
    return err;
}

/* ====================================================================
 *  Token bootstrap: discover-then-claim (default) or register (opt-in)
 * ==================================================================== */

/* Build the JSON body for /device/discover and /device/register. Both
 * endpoints accept the same fields: identity + capability claim. */
static char *build_identity_body(const char *device_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    char mac[13] = {0};
    if (get_mac_hex(mac, sizeof(mac)) != ESP_OK) {
        ESP_LOGW(TAG, "could not read MAC; discover/register will likely fail");
    }
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "kind",       DEVICE_KIND);
    cJSON_AddNumberToObject(root, "panel_w",    EPD_WIDTH);
    cJSON_AddNumberToObject(root, "panel_h",    EPD_HEIGHT);
    cJSON_AddStringToObject(root, "fw_version", FW_VERSION);
    cJSON_AddStringToObject(root, "mac",        mac);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Pick up config.sleep_interval_s from a register/discover/status
 * response and persist it. The server's config schema may grow more
 * keys over time -- we treat the rest as forward-compat and ignore. */
static void apply_response_config(cJSON *response) {
    if (!response) return;
    cJSON *config = cJSON_GetObjectItemCaseSensitive(response, "config");
    if (!cJSON_IsObject(config)) return;
    cJSON *sis = cJSON_GetObjectItemCaseSensitive(config, "sleep_interval_s");
    if (!cJSON_IsNumber(sis)) return;
    int v = sis->valueint;
    if (v < SLEEP_INTERVAL_MIN_S || v > SLEEP_INTERVAL_MAX_S) {
        ESP_LOGW(TAG, "sleep_interval_s=%d out of bounds; ignoring", v);
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_SLEEP_S, v);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved sleep_interval_s=%d to NVS (from response.config)", v);
}

/* server_time is a FLOAT (server uses time.time()), e.g. 1781941884.34;
 * cJSON's valuedouble preserves it across the parse. */
static void apply_server_time_from_response(cJSON *response) {
    if (!response) return;
    cJSON *st = cJSON_GetObjectItemCaseSensitive(response, "server_time");
    if (cJSON_IsNumber(st)) {
        apply_server_time(st->valuedouble);
    }
}

/* Returns ESP_OK if NVS now has a device_token (either it already did,
 * or we just obtained one). Returns the discover-retry suggestion via
 * out_retry_after_s when the server says "registered: false". */
static esp_err_t ensure_device_token(const char *server_url,
                                     const char *device_id,
                                     char *token_buf, size_t token_buf_sz,
                                     char *device_id_buf, size_t device_id_buf_sz,
                                     int *out_retry_after_s) {
    *out_retry_after_s = 0;

    /* Fast path: token already in NVS. */
    if (nvs_get_str_buf(NVS_NS_REST, NVS_KEY_DEVICE_TOKEN,
                         token_buf, token_buf_sz) == ESP_OK &&
        token_buf[0] != '\0') {
        return ESP_OK;
    }
    token_buf[0] = '\0';

    /* secrets.h shortcut: bake a known-good token into the build to
     * skip the discover/register dance entirely. Persist to NVS so the
     * next wake hits the fast path above instead of paying the
     * strlen/memcpy again. If the server has since rotated it, /frame
     * will 401, we'll wipe it from NVS, and the next wake will fall
     * through to discover/register normally. */
    if (REST_DEFAULT_DEVICE_TOKEN[0] != '\0') {
        strncpy(token_buf, REST_DEFAULT_DEVICE_TOKEN, token_buf_sz - 1);
        token_buf[token_buf_sz - 1] = '\0';
        nvs_set_str_buf(NVS_NS_REST, NVS_KEY_DEVICE_TOKEN, token_buf);
        ESP_LOGI(TAG, "device_token primed from secrets.h");
        return ESP_OK;
    }

    char *body = build_identity_body(device_id);
    if (!body) return ESP_ERR_NO_MEM;

    char pair[32] = {0};
    nvs_get_str_buf(NVS_NS_REST, NVS_KEY_PAIRING_CODE, pair, sizeof(pair));
    if (pair[0] == '\0' && REST_DEFAULT_PAIRING_CODE[0] != '\0') {
        strncpy(pair, REST_DEFAULT_PAIRING_CODE, sizeof(pair) - 1);
        ESP_LOGI(TAG, "pairing_code from secrets.h; will hit /register");
    }

    bool use_register = (pair[0] != '\0');
    char url[256];
    esp_err_t err = build_url(url, sizeof(url), server_url,
                              use_register ? "/api/v1/device/register"
                                           : "/api/v1/device/discover");
    if (err != ESP_OK) { free(body); return err; }

    body_capture_t cap = {0};
    int status = 0;
    err = http_request(HTTP_METHOD_POST, url,
                       NULL,                                /* no bearer yet */
                       use_register ? pair : NULL,          /* X-Pairing-Code */
                       NULL,
                       body,
                       &status, &cap);
    free(body);

    if (err != ESP_OK) {
        body_capture_free(&cap);
        return err;
    }

    /* ---- Error paths ---- */

    if (status == 429) {
        /* Rate-limited -- only register hits this. */
        ESP_LOGW(TAG, "register rate-limited (429); will retry next wake");
        body_capture_free(&cap);
        *out_retry_after_s = 3600;   /* be polite */
        return ESP_FAIL;
    }
    if (status == 403 && use_register) {
        ESP_LOGE(TAG, "pairing code invalid/expired (403); deep-sleep 1 h");
        body_capture_free(&cap);
        *out_retry_after_s = 3600;
        return ESP_FAIL;
    }
    if (status == 400) {
        ESP_LOGE(TAG, "register/discover rejected as 400 (body=%.*s)",
                 (int) cap.len, cap.buf ? cap.buf : "");
        body_capture_free(&cap);
        *out_retry_after_s = 3600;
        return ESP_FAIL;
    }
    if (status != 200 && status != 201) {
        ESP_LOGW(TAG, "register/discover returned %d", status);
        body_capture_free(&cap);
        return ESP_FAIL;
    }

    /* ---- Success: parse the JSON response ---- */

    cJSON *root = cJSON_Parse(cap.buf ? cap.buf : "");
    body_capture_free(&cap);
    if (!root) {
        ESP_LOGE(TAG, "register/discover response wasn't valid JSON");
        return ESP_FAIL;
    }

    apply_server_time_from_response(root);

    if (!use_register) {
        /* discover: two response shapes. */
        cJSON *registered = cJSON_GetObjectItemCaseSensitive(root, "registered");
        if (cJSON_IsBool(registered) && !cJSON_IsTrue(registered)) {
            cJSON *ra = cJSON_GetObjectItemCaseSensitive(root, "retry_after_s");
            int retry = (cJSON_IsNumber(ra) && ra->valueint > 0)
                            ? ra->valueint
                            : REST_DISCOVER_RETRY_DEFAULT_S;
            ESP_LOGI(TAG, "device discovered but not claimed yet; "
                          "retry in %d s", retry);
            *out_retry_after_s = retry;
            cJSON_Delete(root);
            return ESP_ERR_NOT_FOUND;
        }
    }

    cJSON *tok = cJSON_GetObjectItemCaseSensitive(root, "device_token");
    if (!cJSON_IsString(tok) || !tok->valuestring || !*tok->valuestring) {
        ESP_LOGE(TAG, "register/discover succeeded but response had no device_token");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strncpy(token_buf, tok->valuestring, token_buf_sz - 1);
    token_buf[token_buf_sz - 1] = '\0';
    nvs_set_str_buf(NVS_NS_REST, NVS_KEY_DEVICE_TOKEN, token_buf);

    /* Server's canonical device_id wins -- it may differ from what we
     * sent if our local id is stale/renamed. The token is bound to the
     * canonical id; use it for the /frame and /status URLs. */
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (cJSON_IsString(id) && id->valuestring && device_id_buf) {
        strncpy(device_id_buf, id->valuestring, device_id_buf_sz - 1);
        device_id_buf[device_id_buf_sz - 1] = '\0';
        ESP_LOGI(TAG, "registered/claimed; canonical device_id='%s'",
                 device_id_buf);
    }

    /* Pairing code is single-use after a successful register. */
    if (use_register) {
        nvs_erase_key_quiet(NVS_NS_REST, NVS_KEY_PAIRING_CODE);
    }
    apply_response_config(root);

    cJSON_Delete(root);
    return ESP_OK;
}

/* ====================================================================
 *  Frame fetch + paint
 * ==================================================================== */

/* On 200 the response body is JSON {url, format, panel_w, panel_h,
 * render_id, renderer_id}; on 304 the panel is current; on 204 the
 * server hasn't rendered anything yet. Returns ESP_OK regardless of
 * which path was taken; out_painted is true if the panel was refreshed
 * (caller persists the new ETag in that case). */
static esp_err_t fetch_and_paint(const char *server_url,
                                 const char *device_id,
                                 const char *bearer_token) {
    char if_none_match[96] = {0};
    nvs_get_str_buf(NVS_NS_REST, NVS_KEY_FRAME_ETAG,
                    if_none_match, sizeof(if_none_match));

    char url[256];
    esp_err_t err = build_url(url, sizeof(url), server_url,
                              "/api/v1/device/%s/frame", device_id);
    if (err != ESP_OK) return err;

    body_capture_t cap = {0};
    int status = 0;
    err = http_request(HTTP_METHOD_GET, url, bearer_token, NULL,
                       if_none_match, NULL, &status, &cap);
    if (err != ESP_OK) { body_capture_free(&cap); return err; }

    if (status == 304) {
        ESP_LOGI(TAG, "frame unchanged (304); skipping paint");
        body_capture_free(&cap);
        return ESP_OK;
    }
    if (status == 204) {
        ESP_LOGI(TAG, "no frame rendered yet (204); skipping paint");
        body_capture_free(&cap);
        return ESP_OK;
    }
    if (status == 401) {
        ESP_LOGE(TAG, "frame GET 401; wiping device_token");
        nvs_erase_key_quiet(NVS_NS_REST, NVS_KEY_DEVICE_TOKEN);
        body_capture_free(&cap);
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "frame GET returned %d; skipping paint", status);
        body_capture_free(&cap);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(cap.buf ? cap.buf : "");
    if (!root) {
        ESP_LOGE(TAG, "frame response wasn't valid JSON");
        body_capture_free(&cap);
        return ESP_FAIL;
    }
    cJSON *url_node = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(url_node) || !url_node->valuestring) {
        ESP_LOGE(TAG, "frame response had no url field");
        cJSON_Delete(root);
        body_capture_free(&cap);
        return ESP_FAIL;
    }
    char frame_url[320];
    strncpy(frame_url, url_node->valuestring, sizeof(frame_url) - 1);
    frame_url[sizeof(frame_url) - 1] = '\0';
    cJSON_Delete(root);
    /* keep the captured ETag (in cap.etag) for after a successful paint. */

    /* Download the .bin via the existing image_fetcher (PSRAM-backed,
     * chunked, 4 MB cap -- way more than the 192 KB this panel uses). */
    fetched_image_t img;
    esp_err_t fetch_err = image_fetch(frame_url, &img);
    if (fetch_err != ESP_OK) {
        ESP_LOGE(TAG, "frame body fetch failed: %s", esp_err_to_name(fetch_err));
        body_capture_free(&cap);
        return fetch_err;
    }

    uint8_t *frame = NULL;
    esp_err_t decode_err = image_decode_to_frame(&img, frame_url, &frame);
    free(img.data);
    if (decode_err != ESP_OK || !frame) {
        ESP_LOGE(TAG, "frame decode failed: %s", esp_err_to_name(decode_err));
        body_capture_free(&cap);
        return decode_err;
    }

    ESP_ERROR_CHECK(epd_port_init());
    epd_display(frame);
    epd_sleep();
    free(frame);

    /* Persist the new ETag so the next wake can short-circuit at 304. */
    if (cap.etag[0]) {
        nvs_set_str_buf(NVS_NS_REST, NVS_KEY_FRAME_ETAG, cap.etag);
    }
    body_capture_free(&cap);
    ESP_LOGI(TAG, "frame painted; etag cached");
    return ESP_OK;
}

/* ====================================================================
 *  Status POST
 * ==================================================================== */

static int post_status(const char *server_url, const char *device_id,
                       const char *bearer_token,
                       int sleep_s, esp_reset_reason_t reset_reason) {
    char hb[320];
    time_t now = time(NULL);
    time_t sleep_until = (now > 1700000000LL && now < 2200000000LL)
                            ? (now + sleep_s) : 0;
    heartbeat_format_json(hb, sizeof hb, sleep_s, reset_reason, sleep_until);

    char url[256];
    if (build_url(url, sizeof(url), server_url,
                  "/api/v1/device/%s/status", device_id) != ESP_OK) {
        return REST_NEXT_POLL_FALLBACK_S;
    }

    body_capture_t cap = {0};
    int status = 0;
    esp_err_t err = http_request(HTTP_METHOD_POST, url, bearer_token, NULL,
                                 NULL, hb, &status, &cap);
    if (err != ESP_OK) {
        body_capture_free(&cap);
        return REST_NEXT_POLL_FALLBACK_S;
    }
    if (status == 401) {
        ESP_LOGE(TAG, "status POST 401; wiping device_token + sleeping 1 h");
        nvs_erase_key_quiet(NVS_NS_REST, NVS_KEY_DEVICE_TOKEN);
        body_capture_free(&cap);
        return 3600;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "status POST returned %d", status);
        body_capture_free(&cap);
        return REST_NEXT_POLL_FALLBACK_S;
    }

    /* Apply config + read next_poll_s. */
    cJSON *root = cJSON_Parse(cap.buf ? cap.buf : "");
    body_capture_free(&cap);
    if (!root) return REST_NEXT_POLL_FALLBACK_S;

    apply_server_time_from_response(root);
    apply_response_config(root);

    int next_poll = REST_NEXT_POLL_FALLBACK_S;
    cJSON *npn = cJSON_GetObjectItemCaseSensitive(root, "next_poll_s");
    if (cJSON_IsNumber(npn)) next_poll = npn->valueint;
    if (next_poll < SLEEP_INTERVAL_MIN_S) next_poll = SLEEP_INTERVAL_MIN_S;
    if (next_poll > SLEEP_INTERVAL_MAX_S) next_poll = SLEEP_INTERVAL_MAX_S;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "status posted; next_poll_s=%d", next_poll);
    return next_poll;
}

/* ====================================================================
 *  Public entry: the wake loop
 * ==================================================================== */

int rest_run_loop(esp_reset_reason_t reset_reason) {
    char server_url[160] = {0};
    nvs_get_str_buf(NVS_NS_REST, NVS_KEY_SERVER_URL,
                    server_url, sizeof(server_url));
    if (!server_url[0]) {
        /* Allow secrets.h compile-time fallback for dev boards. */
        if (REST_DEFAULT_SERVER_URL[0]) {
            strncpy(server_url, REST_DEFAULT_SERVER_URL,
                    sizeof(server_url) - 1);
        }
    }
    if (!server_url[0]) {
        ESP_LOGE(TAG, "transport=rest but no server_url in NVS or secrets.h");
        return SLEEP_INTERVAL_S;   /* keep cycling; user will fix via portal */
    }
    ESP_LOGI(TAG, "transport=rest; server=%s", server_url);

    /* device_id comes from the existing mqtt_config NVS (shared across
     * transports). The discover/register response can return a
     * canonical id different from ours; use whatever wins. */
    mqtt_config_t mc;
    mqtt_config_load(&mc);
    char device_id[64];
    strncpy(device_id, mc.device_id, sizeof(device_id) - 1);
    device_id[sizeof(device_id) - 1] = '\0';

    int retry_after_s = 0;
    char token[160] = {0};
    esp_err_t bootstrap = ensure_device_token(server_url, device_id,
                                              token, sizeof(token),
                                              device_id, sizeof(device_id),
                                              &retry_after_s);
    if (bootstrap == ESP_ERR_NOT_FOUND) {
        /* Discovered but not yet claimed. Sleep retry_after_s; admin
         * has to click Register on Settings -> Devices. */
        return retry_after_s > 0 ? retry_after_s
                                 : REST_DISCOVER_RETRY_DEFAULT_S;
    }
    if (bootstrap != ESP_OK || token[0] == '\0') {
        return retry_after_s > 0 ? retry_after_s : SLEEP_INTERVAL_S;
    }

    (void) fetch_and_paint(server_url, device_id, token);

    /* Sleep duration comes from the status response's next_poll_s. */
    int sleep_s = SLEEP_INTERVAL_S;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, NVS_KEY_SLEEP_S, &v) == ESP_OK &&
            v >= SLEEP_INTERVAL_MIN_S && v <= SLEEP_INTERVAL_MAX_S) {
            sleep_s = v;
        }
        nvs_close(h);
    }

    int next_poll = post_status(server_url, device_id, token,
                                sleep_s, reset_reason);
    return next_poll;
}
