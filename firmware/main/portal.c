// Hotspot setup portal: a tiny web page to enter Wi-Fi details from a phone,
// plus a DNS responder so phones pop the page up automatically.

#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "wifi_mgr.h"
#include "portal.h"
#include "app_config.h"

static const char *TAG = "portal";
static httpd_handle_t server;
static TaskHandle_t dns_task_h;
static volatile bool dns_run;

// ---------------- DNS: answer every query with 192.168.4.1 ----------------

static void dns_task(void *arg)
{
    int dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (dns_sock < 0 || bind(dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        goto done;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(dns_sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        // find end of the question section (name + type + class)
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;
        if (p > n) continue;
        buf[2] = 0x81; buf[3] = 0x80;          // response, recursion available, no error
        buf[6] = 0; buf[7] = 1;                // one answer
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t ans[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
        memcpy(buf + p, ans, sizeof(ans));
        sendto(dns_sock, buf, p + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
done:
    if (dns_sock >= 0) close(dns_sock);
    dns_task_h = NULL;
    vTaskDelete(NULL);
}

// ---------------- HTTP ----------------

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((int)s[1]) && isxdigit((int)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

// Escapes into a buffer and sends it as one chunk (SSIDs are at most 32 bytes)
static void html_escape_append(httpd_req_t *req, const char *s)
{
    char out[32 * 6 + 1];
    size_t o = 0;
    for (; *s && o + 6 < sizeof(out); s++) {
        const char *rep = NULL;
        switch (*s) {
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '&': rep = "&amp;"; break;
        case '"': rep = "&quot;"; break;
        }
        if (rep) {
            size_t n = strlen(rep);
            memcpy(out + o, rep, n);
            o += n;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = 0;
    if (o) httpd_resp_sendstr_chunk(req, out);
}

static const char *PAGE_HEAD =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP Box Wi-Fi</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0f1115;color:#e8eaf0;margin:0;padding:24px}"
    "h1{font-size:20px}label{display:block;margin:16px 0 6px;color:#9aa3b2}"
    "input,select{width:100%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:8px;"
    "border:1px solid #333;background:#1b1f27;color:#e8eaf0}"
    "button{margin-top:20px;width:100%;padding:14px;font-size:16px;border:0;border-radius:8px;"
    "background:#3b82f6;color:#fff}p{color:#9aa3b2}</style></head><body>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Connect your ESP Box to Wi-Fi</h1><form method=post action=/save>"
                                  "<label>Network</label><input name=ssid list=nets required maxlength=32 "
                                  "placeholder='Pick or type the network name'><datalist id=nets>");
    wifi_ap_record_t recs[20];
    int n = wifi_mgr_last_scan(recs, 20);
    for (int i = 0; i < n; i++) {
        if (!recs[i].ssid[0]) continue;
        httpd_resp_sendstr_chunk(req, "<option value=\"");
        html_escape_append(req, (const char *)recs[i].ssid);
        httpd_resp_sendstr_chunk(req, "\">");
    }
    httpd_resp_sendstr_chunk(req, "</datalist><label>Password</label>"
                                  "<input name=pass type=text maxlength=63 autocomplete=off "
                                  "placeholder='Leave empty for open networks'>"
                                  "<button type=submit>Save and connect</button></form>"
                                  "<p>The box remembers this network. You can also manage networks "
                                  "later from the setup webpage.</p></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[256] = { 0 };
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    char ssid[96] = { 0 }, pass[200] = { 0 };
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    ssid[32] = 0;
    pass[64] = 0;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    if (!ssid[0]) {
        httpd_resp_sendstr_chunk(req, "<h1>Network name is required</h1><p><a href=/ style=color:#3b82f6>Back</a></p>");
    } else {
        httpd_resp_sendstr_chunk(req, "<h1>Saved</h1><p>The box is connecting to <b>");
        html_escape_append(req, ssid);
        httpd_resp_sendstr_chunk(req, "</b>. Watch its screen. If it can't connect, this hotspot "
                                      "comes back and you can try again.</p>");
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    if (ssid[0]) wifi_mgr_add_local(ssid, pass);
    return ESP_OK;
}

static esp_err_t redirect_any(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" SETUP_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void portal_start(void)
{
    if (server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        server = NULL;
        return;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_any);

    // A DNS task from a previous portal_stop() may still be closing its socket (up to 1 s)
    for (int i = 0; i < 30 && dns_task_h; i++) vTaskDelay(pdMS_TO_TICKS(50));
    dns_run = true;
    if (!dns_task_h) xTaskCreate(dns_task, "dns", 3072, NULL, 4, &dns_task_h);
}

void portal_stop(void)
{
    dns_run = false;
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}
