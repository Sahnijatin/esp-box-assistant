#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include "app_config.h"
#include "cloud.h"
#include "wifi_mgr.h"
#include "audio.h"
#include "ui.h"
#include "ota.h"
#include "assistant.h"

static const char *TAG = "cloud";

static char dev_id[13];
static char token[65];
static char ota_failed[33];
static volatile bool paired;
static TaskHandle_t task_h;

typedef struct {
    int status;
    char *body;
    int len;
} resp_t;

// ---------------- identity ----------------

static void load_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(dev_id, sizeof(dev_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t h;
    if (nvs_open("dev", NVS_READWRITE, &h) != ESP_OK) return;
    size_t sz = sizeof(token);
    if (nvs_get_str(h, "token", token, &sz) != ESP_OK || strlen(token) != 64) {
        uint8_t rnd[32];
        esp_fill_random(rnd, sizeof(rnd));
        for (int i = 0; i < 32; i++) sprintf(token + i * 2, "%02x", rnd[i]);
        nvs_set_str(h, "token", token);
        nvs_commit(h);
        ESP_LOGI(TAG, "new device token created");
    }
    sz = sizeof(ota_failed);
    if (nvs_get_str(h, "ota_fail", ota_failed, &sz) != ESP_OK) ota_failed[0] = 0;
    nvs_close(h);

    // If the last update was rolled back, report that version so it isn't offered again
    const esp_partition_t *inv = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (inv && esp_ota_get_partition_description(inv, &d) == ESP_OK &&
        strcmp(d.version, esp_app_get_description()->version) != 0) {
        strlcpy(ota_failed, d.version, sizeof(ota_failed));
        ESP_LOGW(TAG, "previous update %s was rolled back", ota_failed);
    }
}

// ---------------- HTTP helpers ----------------

static esp_http_client_handle_t client_for(const char *path, int timeout_ms)
{
    char url[160];
    snprintf(url, sizeof(url), API_BASE "%s", path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "X-Device-Id", dev_id);
    esp_http_client_set_header(c, "X-Device-Token", token);
    return c;
}

static bool write_all(esp_http_client_handle_t c, const uint8_t *data, size_t len)
{
    while (len) {
        int w = esp_http_client_write(c, (const char *)data, len > 4096 ? 4096 : len);
        if (w <= 0) return false;
        data += w;
        len -= w;
    }
    return true;
}

static void read_body(esp_http_client_handle_t c, resp_t *r)
{
    int cap = 4096;
    r->body = malloc(cap + 1);
    r->len = 0;
    if (!r->body) {
        r->body = strdup("");
        return;
    }
    for (;;) {
        if (r->len == cap) {
            if (cap >= 32768) break;
            char *nb = realloc(r->body, cap * 2 + 1);
            if (!nb) break;
            r->body = nb;
            cap *= 2;
        }
        int n = esp_http_client_read(c, r->body + r->len, cap - r->len);
        if (n <= 0) break;
        r->len += n;
    }
    r->body[r->len] = 0;
}

// POST (head + body) and read the whole response
static bool post(const char *path, const char *ctype, const uint8_t *head, size_t head_len,
                 const uint8_t *body, size_t body_len, int timeout_ms, resp_t *r)
{
    memset(r, 0, sizeof(*r));
    esp_http_client_handle_t c = client_for(path, timeout_ms);
    esp_http_client_set_header(c, "Content-Type", ctype);
    bool ok = false;
    if (esp_http_client_open(c, head_len + body_len) == ESP_OK &&
        (!head_len || write_all(c, head, head_len)) &&
        (!body_len || write_all(c, body, body_len))) {
        esp_http_client_fetch_headers(c);
        r->status = esp_http_client_get_status_code(c);
        read_body(c, r);
        ok = true;
    } else {
        ESP_LOGW(TAG, "POST %s failed", path);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static char *dupstr(const char *s) { return strdup(s ? s : ""); }

// ---------------- ask / speak ----------------

bool cloud_ask(const uint8_t *wav, size_t len, char **question, char **answer, char **speech,
               bool *render, char **error)
{
    *question = *answer = *speech = *error = NULL;
    *render = false;
    resp_t r;
    if (!post("/device/ask", "audio/wav", NULL, 0, wav, len, 60000, &r)) {
        *error = dupstr("Couldn't reach the server.");
        return false;
    }
    cJSON *j = cJSON_Parse(r.body);
    bool ok = false;
    if (!j) {
        *error = dupstr("Server sent a bad reply.");
    } else {
        const char *q = cJSON_GetStringValue(cJSON_GetObjectItem(j, "question"));
        const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(j, "answer"));
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(j, "error"));
        *question = dupstr(q);
        if (e && *e) {
            *error = dupstr(e);
        } else if (r.status != 200 || !a) {
            bool unpaired = r.status == 401 || r.status == 403;
            *error = dupstr(unpaired ? "This box isn't paired." : "Server error.");
            if (unpaired) cloud_poll_now();   // check-in shows the pairing code or blocked state
        } else {
            const char *sp = cJSON_GetStringValue(cJSON_GetObjectItem(j, "speech"));
            *answer = dupstr(a);
            *speech = dupstr(sp && *sp ? sp : a);
            *render = cJSON_IsTrue(cJSON_GetObjectItem(j, "render"));
            ok = true;
        }
        cJSON_Delete(j);
    }
    free(r.body);
    return ok;
}

static bool read_exact(esp_http_client_handle_t c, uint8_t *dst, int len)
{
    while (len > 0) {
        int n = esp_http_client_read(c, (char *)dst, len);
        if (n <= 0) return false;
        dst += n;
        len -= n;
    }
    return true;
}

bool cloud_render(const char *question, const char *answer, uint8_t **rgb565, int *w, int *h)
{
    *rgb565 = NULL;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "question", question ? question : "");
    cJSON_AddStringToObject(j, "answer", answer ? answer : "");
    cJSON_AddNumberToObject(j, "width", 300);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    esp_http_client_handle_t c = client_for("/device/render", 20000);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    bool ok = false;
    size_t blen = strlen(body);
    uint8_t hdr[4];
    if (esp_http_client_open(c, blen) == ESP_OK && write_all(c, (uint8_t *)body, blen)) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200 && read_exact(c, hdr, 4)) {
            *w = hdr[0] | (hdr[1] << 8);
            *h = hdr[2] | (hdr[3] << 8);
            size_t sz = (size_t)*w * *h * 2;
            if (*w > 0 && *w <= 320 && *h > 0 && *h <= 1200) {
                *rgb565 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
                if (*rgb565 && read_exact(c, *rgb565, sz)) ok = true;
            }
        }
    }
    if (!ok && *rgb565) { free(*rgb565); *rgb565 = NULL; }
    if (!ok) ESP_LOGW(TAG, "render failed");
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    return ok;
}

bool cloud_speak(const char *text, volatile bool *stop)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "text", text);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    esp_http_client_handle_t c = client_for("/device/tts", 30000);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    bool ok = false;
    size_t blen = strlen(body);
    if (esp_http_client_open(c, blen) == ESP_OK && write_all(c, (uint8_t *)body, blen)) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        uint8_t *buf = status == 200 ? heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : NULL;
        if (buf) {
            int carry = -1;    // odd byte left from the previous chunk
            audio_play_begin(TTS_SAMPLE_RATE);
            for (;;) {
                if (stop && *stop) break;
                int off = 0;
                if (carry >= 0) { buf[0] = (uint8_t)carry; off = 1; carry = -1; }
                int n = esp_http_client_read(c, (char *)buf + off, 4096 - off);
                if (n <= 0) break;
                n += off;
                if (n & 1) carry = buf[--n];
                audio_play_write(buf, n);
            }
            audio_play_end();
            free(buf);
            ok = true;
        } else if (status != 200) {
            resp_t r;
            read_body(c, &r);
            ESP_LOGW(TAG, "tts failed %d: %s", status, r.body);
            free(r.body);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    return ok;
}

// ---------------- check-in loop ----------------

static void mark_app_valid(void)
{
    static bool done;
    if (done) return;
    done = true;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed working");
    }
}

static bool app_pending_verify(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

static int apply_config(const cJSON *cfg)
{
    int poll = 30;
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(cfg, "volume"))) audio_set_volume(v->valueint);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(cfg, "brightness"))) ui_set_brightness(v->valueint);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(cfg, "poll_seconds"))) poll = v->valueint;
    wifi_mgr_set_cloud_list(cJSON_GetObjectItem(cfg, "wifi"));

    const cJSON *up = cJSON_GetObjectItem(cfg, "update");
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(up, "version"));
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(up, "url"));
    if (ver && url && strcmp(ver, ota_failed) != 0) {
        if (assistant_busy()) return 5;   // try again once the conversation is over
        ESP_LOGI(TAG, "update available: %s", ver);
        // A Wi-Fi blip shouldn't rule a build out: give up on it after 3 failed attempts
        static char tried_ver[33];
        static int tries;
        if (strcmp(tried_ver, ver) != 0) {
            strlcpy(tried_ver, ver, sizeof(tried_ver));
            tries = 0;
        }
        if (!ota_run(url, ver) && ++tries >= 3) {   // ota_run only returns on failure
            strlcpy(ota_failed, ver, sizeof(ota_failed));
            nvs_handle_t h;
            if (nvs_open("dev", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, "ota_fail", ota_failed);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    return poll;
}

static void cloud_task(void *arg)
{
    int64_t fail_since = 0;
    for (;;) {
        wifi_mgr_wait_connected(-1);

        char ssid[33], ip[16];
        int rssi;
        wifi_mgr_info(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", dev_id);
        cJSON_AddStringToObject(j, "token", token);
        cJSON_AddStringToObject(j, "fw", esp_app_get_description()->version);
        cJSON_AddStringToObject(j, "ip", ip);
        cJSON_AddStringToObject(j, "ssid", ssid);
        cJSON_AddNumberToObject(j, "rssi", rssi);
        cJSON_AddNumberToObject(j, "heap", esp_get_free_heap_size());
        if (ota_failed[0]) cJSON_AddStringToObject(j, "ota_failed", ota_failed);
        char *body = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);

        resp_t r;
        bool ok = post("/device/hello", "application/json", NULL, 0, (uint8_t *)body, strlen(body), 15000, &r);
        free(body);
        cJSON *resp = ok ? cJSON_Parse(r.body) : NULL;
        const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "status"));
        int wait_s = 10;

        if (status) {
            fail_since = 0;
            mark_app_valid();
            if (strcmp(status, "pending") == 0) {
                paired = false;
                const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "pair_code"));
                ui_show_pairing(code ? code : "------");
                ui_set_mode(UI_BLOCKED, "Pair this box from the setup page");
                wait_s = 4;
            } else if (strcmp(status, "blocked") == 0) {
                paired = false;
                ui_hide_pairing();
                ui_set_mode(UI_BLOCKED, "This box is blocked on the setup page");
                wait_s = 60;
            } else {
                if (!paired) {
                    paired = true;
                    ui_hide_pairing();
                    if (!assistant_busy()) ui_set_mode(UI_IDLE, NULL);
                }
                wait_s = apply_config(cJSON_GetObjectItem(resp, "config"));
            }
        } else {
            ESP_LOGW(TAG, "check-in failed (HTTP %d)", ok ? r.status : -1);
            int64_t now = esp_timer_get_time();
            if (!fail_since) fail_since = now;
            // New firmware that can't reach the server for 3 minutes on working Wi-Fi: roll back
            if (app_pending_verify() && now - fail_since > 180LL * 1000000) {
                ESP_LOGE(TAG, "new firmware can't reach the server, rolling back");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
            if (!paired) ui_set_mode(UI_BLOCKED, "Can't reach the server, retrying...");
        }
        if (resp) cJSON_Delete(resp);
        if (ok) free(r.body);

        if (wait_s < 3) wait_s = 3;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));
    }
}

void cloud_start(void)
{
    load_identity();
    ESP_LOGI(TAG, "device id %s", dev_id);
    xTaskCreatePinnedToCore(cloud_task, "cloud", 8192, NULL, 5, &task_h, 0);
}

bool cloud_paired(void) { return paired; }

void cloud_poll_now(void)
{
    if (task_h) xTaskNotifyGive(task_h);
}
