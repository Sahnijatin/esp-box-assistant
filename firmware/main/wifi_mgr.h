#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_wifi_types.h"

// Wi-Fi manager
//  - remembers up to WIFI_MAX_NETS networks in NVS
//  - networks come from two places: the setup webpage (pushed in config, "cloud")
//    and the box itself (touchscreen or hotspot portal, "local")
//  - if no saved network works, it opens the touchscreen picker and a setup hotspot

#define WIFI_MAX_NETS 12

void wifi_mgr_init(void);
bool wifi_mgr_ready(void);          // true once wifi_mgr_init() has finished
bool wifi_mgr_connected(void);
bool wifi_mgr_wait_connected(int timeout_ms);
void wifi_mgr_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi);

// From the touchscreen / portal: save and connect right away
void wifi_mgr_add_local(const char *ssid, const char *password);
// From cloud config: replace the cloud-provided networks (local ones are kept)
void wifi_mgr_set_cloud_list(const cJSON *arr);

// Manual open of the setup screen (tap on the Wi-Fi bar)
void wifi_mgr_open_setup(void);
void wifi_mgr_close_setup(void);
void wifi_mgr_rescan(void);

// Last scan results (for the hotspot portal page)
int wifi_mgr_last_scan(wifi_ap_record_t *out, int max);
const char *wifi_mgr_ap_name(void);
