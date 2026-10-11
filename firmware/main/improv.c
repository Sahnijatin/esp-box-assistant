// Improv Wi-Fi Serial (protocol version 1) on the native USB port.
//
// Packet: "IMPROV" | version | type | length | data[length] | checksum
// checksum = low byte of the sum of every byte before it.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "driver/usb_serial_jtag.h"
#include "app_config.h"
#include "wifi_mgr.h"
#include "improv.h"

static const char *TAG = "improv";

#define IMPROV_VERSION      1

enum { T_STATE = 0x01, T_ERROR = 0x02, T_RPC = 0x03, T_RPC_RESULT = 0x04 };
enum { S_AUTHORIZED = 0x02, S_PROVISIONING = 0x03, S_PROVISIONED = 0x04 };
enum { E_NONE = 0x00, E_INVALID_RPC = 0x01, E_UNKNOWN_RPC = 0x02, E_UNABLE_TO_CONNECT = 0x03 };
enum { C_WIFI = 0x01, C_STATE = 0x02, C_INFO = 0x03, C_SCAN = 0x04 };

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t p[6 + 3 + 255 + 2];
    memcpy(p, "IMPROV", 6);
    p[6] = IMPROV_VERSION;
    p[7] = type;
    p[8] = len;
    if (len) memcpy(p + 9, data, len);
    uint8_t sum = 0;
    for (int i = 0; i < 9 + len; i++) sum += p[i];
    p[9 + len] = sum;
    p[10 + len] = '\n';
    // Short timeout: with no computer attached nothing drains the USB buffer
    usb_serial_jtag_write_bytes(p, 11 + len, pdMS_TO_TICKS(50));
}

static void send_state(uint8_t s) { send_packet(T_STATE, &s, 1); }
static void send_error(uint8_t e) { send_packet(T_ERROR, &e, 1); }

// RPC result: command | length | (string length | string)...
static void send_result(uint8_t cmd, const char *const *strs, int n)
{
    uint8_t d[255];
    int o = 2;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(strs[i]);
        if (o + 1 + l > sizeof(d)) break;
        d[o++] = (uint8_t)l;
        memcpy(d + o, strs[i], l);
        o += l;
    }
    d[0] = cmd;
    d[1] = (uint8_t)(o - 2);
    send_packet(T_RPC_RESULT, d, (uint8_t)o);
}

static uint8_t current_state(void)
{
    return wifi_mgr_ready() && wifi_mgr_connected() ? S_PROVISIONED : S_AUTHORIZED;
}

static void wait_wifi_ready(void)
{
    while (!wifi_mgr_ready()) vTaskDelay(pdMS_TO_TICKS(50));
}

static void do_wifi(const uint8_t *d, int len)
{
    // ssid length | ssid | password length | password
    if (len < 2 || d[0] > 32 || 1 + d[0] + 1 > len) { send_error(E_INVALID_RPC); return; }
    int sl = d[0], pl = d[1 + sl];
    if (pl > 64 || 2 + sl + pl > len) { send_error(E_INVALID_RPC); return; }
    char ssid[33], pass[65];
    memcpy(ssid, d + 1, sl); ssid[sl] = 0;
    memcpy(pass, d + 2 + sl, pl); pass[pl] = 0;
    if (!ssid[0]) { send_error(E_INVALID_RPC); return; }

    ESP_LOGI(TAG, "Wi-Fi from installer: %s", ssid);
    send_error(E_NONE);
    send_state(S_PROVISIONING);
    wait_wifi_ready();
    wifi_mgr_add_local(ssid, pass);

    // Give the connection attempt time to finish (try_connect waits up to 15 s)
    vTaskDelay(pdMS_TO_TICKS(500));
    for (int i = 0; i < 120; i++) {
        char cur[33];
        if (wifi_mgr_connected()) {
            wifi_mgr_info(cur, sizeof(cur), NULL, 0, NULL);
            if (strcmp(cur, ssid) == 0) {
                send_state(S_PROVISIONED);
                const char *url[] = { SETUP_URL };   // "Next" button: pair it on the setup page
                send_result(C_WIFI, url, 1);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    send_error(E_UNABLE_TO_CONNECT);
    send_state(current_state());
}

static void do_scan(void)
{
    wait_wifi_ready();
    static wifi_ap_record_t recs[20];
    int n = wifi_mgr_last_scan(recs, 20);
    if (n == 0) {
        wifi_mgr_rescan();
        for (int i = 0; i < 40 && n == 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            n = wifi_mgr_last_scan(recs, 20);
        }
    }
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < i && !dup; k++) dup = strcmp(ssid, (const char *)recs[k].ssid) == 0;
        if (dup) continue;
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", recs[i].rssi);
        const char *s[] = { ssid, rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES" };
        send_result(C_SCAN, s, 3);
    }
    send_result(C_SCAN, NULL, 0);   // end of list
}

static void handle_rpc(const uint8_t *d, int len)
{
    if (len < 2 || 2 + d[1] > len) { send_error(E_INVALID_RPC); return; }
    uint8_t cmd = d[0];
    const uint8_t *arg = d + 2;
    int alen = d[1];
    switch (cmd) {
    case C_WIFI:
        do_wifi(arg, alen);
        break;
    case C_STATE:
        send_state(current_state());
        if (current_state() == S_PROVISIONED) {
            const char *url[] = { SETUP_URL };
            send_result(C_STATE, url, 1);
        }
        break;
    case C_INFO: {
        const char *s[] = { "ESP Box Assistant", esp_app_get_description()->version, "ESP32-S3",
                            wifi_mgr_ap_name() };
        send_result(C_INFO, s, 4);
        break;
    }
    case C_SCAN:
        do_scan();
        break;
    default:
        send_error(E_UNKNOWN_RPC);
    }
}

static void improv_task(void *arg)
{
    static uint8_t pkt[6 + 3 + 255 + 1];
    int pos = 0;
    uint8_t b;
    for (;;) {
        if (usb_serial_jtag_read_bytes(&b, 1, portMAX_DELAY) != 1) continue;
        // Sync on the "IMPROV" header; anything else is ignored
        if (pos < 6) {
            if (b == (uint8_t)"IMPROV"[pos]) pkt[pos++] = b;
            else pos = (b == 'I') ? (pkt[0] = b, 1) : 0;
            continue;
        }
        pkt[pos++] = b;
        if (pos < 9) continue;
        int len = pkt[8];
        if (pos < 10 + len) continue;
        uint8_t sum = 0;
        for (int i = 0; i < 9 + len; i++) sum += pkt[i];
        if (pkt[6] != IMPROV_VERSION || sum != pkt[9 + len]) {
            send_error(E_INVALID_RPC);
        } else if (pkt[7] == T_RPC) {
            handle_rpc(pkt + 9, len);
        }
        pos = 0;
    }
}

void improv_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 512;
    cfg.tx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB serial driver install failed");
        return;
    }
    xTaskCreate(improv_task, "improv", 4096, NULL, 3, NULL);
}
