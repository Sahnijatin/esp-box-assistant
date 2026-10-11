#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "wifi_mgr.h"
#include "portal.h"
#include "ui.h"
#include "app_config.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED BIT0
#define BIT_FAIL      BIT1
#define BIT_WAKE      BIT2

typedef struct {
    char ssid[33];
    char pass[65];
    int16_t prio;
    uint8_t cloud;
} net_t;

static net_t nets[WIFI_MAX_NETS];
static int net_count;
static SemaphoreHandle_t lock;
static EventGroupHandle_t ev;
static esp_netif_t *sta_netif;
static char ap_name[24];
static bool ap_active, setup_manual, rescan_req, connecting;
static char pending_ssid[33], pending_pass[65];
static bool pending;
static char cur_ssid[33];

#define SCAN_MAX 20
static wifi_ap_record_t scan_recs[SCAN_MAX];
static uint16_t scan_count;

// ---------------- storage ----------------

static void save_nets(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", nets, sizeof(net_t) * net_count);
    nvs_set_i32(h, "count", net_count);
    nvs_commit(h);
    nvs_close(h);
}

static void load_nets(void)
{
    nvs_handle_t h;
    net_count = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    int32_t c = 0;
    size_t sz = sizeof(nets);
    if (nvs_get_i32(h, "count", &c) == ESP_OK && c > 0 && c <= WIFI_MAX_NETS &&
        nvs_get_blob(h, "nets", nets, &sz) == ESP_OK && sz == sizeof(net_t) * c) {
        net_count = c;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d saved networks", net_count);
}

static int find_net(const char *ssid)
{
    for (int i = 0; i < net_count; i++) if (strcmp(nets[i].ssid, ssid) == 0) return i;
    return -1;
}

void wifi_mgr_add_local(const char *ssid, const char *password)
{
    if (!ssid || !*ssid) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    int i = find_net(ssid);
    if (i < 0) {
        if (net_count == WIFI_MAX_NETS) {   // drop the lowest priority local entry
            int drop = 0;
            for (int k = 1; k < net_count; k++) if (nets[k].prio < nets[drop].prio) drop = k;
            nets[drop] = nets[--net_count];
        }
        i = net_count++;
    }
    strlcpy(nets[i].ssid, ssid, sizeof(nets[i].ssid));
    strlcpy(nets[i].pass, password ? password : "", sizeof(nets[i].pass));
    nets[i].prio = 50;
    nets[i].cloud = 0;
    save_nets();
    strlcpy(pending_ssid, ssid, sizeof(pending_ssid));
    strlcpy(pending_pass, password ? password : "", sizeof(pending_pass));
    pending = true;
    xSemaphoreGive(lock);
    xEventGroupSetBits(ev, BIT_WAKE);
}

void wifi_mgr_set_cloud_list(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) return;
    net_t next[WIFI_MAX_NETS];
    int n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    // cloud networks first
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(it, "ssid"));
        const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(it, "password"));
        const cJSON *pr = cJSON_GetObjectItem(it, "priority");
        if (!s || !*s || n >= WIFI_MAX_NETS) continue;
        memset(&next[n], 0, sizeof(net_t));
        strlcpy(next[n].ssid, s, sizeof(next[n].ssid));
        strlcpy(next[n].pass, p ? p : "", sizeof(next[n].pass));
        next[n].prio = cJSON_IsNumber(pr) ? pr->valueint : 0;
        next[n].cloud = 1;
        n++;
    }
    // keep local networks that the cloud list doesn't mention
    for (int i = 0; i < net_count && n < WIFI_MAX_NETS; i++) {
        if (nets[i].cloud) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (strcmp(next[k].ssid, nets[i].ssid) == 0) dup = true;
        if (!dup) next[n++] = nets[i];
    }
    bool changed = n != net_count || memcmp(next, nets, sizeof(net_t) * n) != 0;
    if (changed) {
        memcpy(nets, next, sizeof(net_t) * n);
        net_count = n;
        save_nets();
        ESP_LOGI(TAG, "network list updated from cloud (%d)", n);
    }
    xSemaphoreGive(lock);
    if (changed && !wifi_mgr_connected()) xEventGroupSetBits(ev, BIT_WAKE);
}

// ---------------- events ----------------

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        bool was_connected = xEventGroupGetBits(ev) & BIT_CONNECTED;
        xEventGroupClearBits(ev, BIT_CONNECTED);
        if (d->reason == WIFI_REASON_ASSOC_LEAVE && !was_connected) return;   // our own disconnect()
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        if (connecting) xEventGroupSetBits(ev, BIT_FAIL);
        if (was_connected) {
            ui_set_wifi(false, NULL);
            xEventGroupSetBits(ev, BIT_WAKE);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(ev, BIT_CONNECTED);
    }
}

// ---------------- helpers ----------------

static void do_scan(void)
{
    wifi_scan_config_t cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return;
    uint16_t n = SCAN_MAX;
    esp_wifi_scan_get_ap_records(&n, scan_recs);
    scan_count = n;
    ESP_LOGI(TAG, "scan: %d networks", n);
    if (ui_wifi_visible()) ui_wifi_set_results(scan_recs, n);
}

static bool try_connect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "connecting to %s", ssid);
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    c.sta.pmf_cfg.capable = true;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(ev, BIT_CONNECTED | BIT_FAIL);
    connecting = true;
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(ev, BIT_CONNECTED | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    connecting = false;
    if (b & BIT_CONNECTED) {
        strlcpy(cur_ssid, ssid, sizeof(cur_ssid));
        return true;
    }
    esp_wifi_disconnect();
    return false;
}

static void start_ap(void)
{
    if (ap_active) return;
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ap_name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    portal_start();
    ap_active = true;
    ESP_LOGI(TAG, "setup hotspot %s started", ap_name);
}

static void stop_ap(void)
{
    if (!ap_active) return;
    portal_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    ap_active = false;
}

static void on_connected(void)
{
    ESP_LOGI(TAG, "connected to %s", cur_ssid);
    ui_set_wifi(true, cur_ssid);
    if (ap_active) {
        ui_wifi_set_message("Connected!");
        vTaskDelay(pdMS_TO_TICKS(800));
        stop_ap();
    }
    if (!setup_manual) ui_wifi_hide();
}

// Try the network the user just entered, then every saved network that's in range.
static bool connect_round(void)
{
    if (pending) {
        char s[33], p[65];
        xSemaphoreTake(lock, portMAX_DELAY);
        strlcpy(s, pending_ssid, sizeof(s));
        strlcpy(p, pending_pass, sizeof(p));
        pending = false;
        xSemaphoreGive(lock);
        if (try_connect(s, p)) {
            setup_manual = false;
            return true;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Couldn't connect to %s. Check the password and try again.", s);
        ui_wifi_set_message(msg);
    }

    do_scan();
    // candidates: saved networks that are visible, best priority then best signal
    int order[WIFI_MAX_NETS], rssi[WIFI_MAX_NETS], n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < net_count; i++) {
        for (int k = 0; k < scan_count; k++) {
            if (strcmp(nets[i].ssid, (const char *)scan_recs[k].ssid) == 0) {
                order[n] = i;
                rssi[n] = scan_recs[k].rssi;
                n++;
                break;
            }
        }
    }
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            net_t *x = &nets[order[a]], *y = &nets[order[b]];
            if (y->prio > x->prio || (y->prio == x->prio && rssi[b] > rssi[a])) {
                int t = order[a]; order[a] = order[b]; order[b] = t;
                t = rssi[a]; rssi[a] = rssi[b]; rssi[b] = t;
            }
        }
    }
    net_t cand[WIFI_MAX_NETS];
    for (int i = 0; i < n; i++) cand[i] = nets[order[i]];
    xSemaphoreGive(lock);

    for (int i = 0; i < n; i++) {
        if (pending) return false;   // user entered something new: handle it first
        if (try_connect(cand[i].ssid, cand[i].pass)) return true;
    }
    return false;
}

static void wifi_task(void *arg)
{
    for (;;) {
        if (xEventGroupGetBits(ev) & BIT_CONNECTED) {
            xEventGroupWaitBits(ev, BIT_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pending) {
                if (connect_round()) on_connected();
                continue;
            }
            if (rescan_req) {
                rescan_req = false;
                do_scan();
            }
            continue;
        }

        ui_set_wifi(false, NULL);
        if (connect_round()) {
            on_connected();
            continue;
        }
        if (!ap_active) {
            start_ap();
            ui_wifi_show(ap_name, false);
            ui_wifi_set_results(scan_recs, scan_count);
        }
        // wait for new details from the screen/portal, or retry saved networks in 30 s
        xEventGroupWaitBits(ev, BIT_WAKE, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
        if (rescan_req && !pending) {
            rescan_req = false;
            do_scan();
            xEventGroupWaitBits(ev, BIT_WAKE, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
        }
    }
}

// ---------------- public ----------------

void wifi_mgr_init(void)
{
    lock = xSemaphoreCreateMutex();
    ev = xEventGroupCreate();
    load_nets();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ap_name, sizeof(ap_name), SOFTAP_PREFIX "%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // The box is mains powered: power save makes audio streaming slow and choppy
    esp_wifi_set_ps(WIFI_PS_NONE);

    xTaskCreate(wifi_task, "wifi_mgr", 6144, NULL, 5, NULL);
}

bool wifi_mgr_connected(void)
{
    return xEventGroupGetBits(ev) & BIT_CONNECTED;
}

bool wifi_mgr_wait_connected(int timeout_ms)
{
    return xEventGroupWaitBits(ev, BIT_CONNECTED, pdFALSE, pdFALSE,
                               timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) & BIT_CONNECTED;
}

void wifi_mgr_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi)
{
    if (ssid) strlcpy(ssid, cur_ssid, ssid_len);
    if (ip) {
        esp_netif_ip_info_t info = { 0 };
        esp_netif_get_ip_info(sta_netif, &info);
        snprintf(ip, ip_len, IPSTR, IP2STR(&info.ip));
    }
    if (rssi) {
        wifi_ap_record_t ap;
        *rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    }
}

void wifi_mgr_open_setup(void)
{
    setup_manual = true;
    ui_wifi_show(ap_name, wifi_mgr_connected());
    ui_wifi_set_results(scan_recs, scan_count);
    rescan_req = true;
    xEventGroupSetBits(ev, BIT_WAKE);
}

void wifi_mgr_close_setup(void)
{
    setup_manual = false;
    if (wifi_mgr_connected()) ui_wifi_hide();
}

void wifi_mgr_rescan(void)
{
    rescan_req = true;
    xEventGroupSetBits(ev, BIT_WAKE);
}

int wifi_mgr_last_scan(wifi_ap_record_t *out, int max)
{
    int n = scan_count < max ? scan_count : max;
    memcpy(out, scan_recs, sizeof(wifi_ap_record_t) * n);
    return n;
}

const char *wifi_mgr_ap_name(void) { return ap_name; }
