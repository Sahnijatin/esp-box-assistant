#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "nvs.h"
#include "ota.h"
#include "ui.h"

static const char *TAG = "ota";

bool ota_run(const char *url, const char *version)
{
    ESP_LOGI(TAG, "updating to %s", version);
    ui_show_ota(version, 0);

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin failed: %s", esp_err_to_name(err));
        goto fail;
    }
    int total = esp_https_ota_get_image_size(h);
    int last_pct = -1;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        int pct = total > 0 ? (int)((int64_t)done * 100 / total) : 0;
        if (pct != last_pct && pct % 2 == 0) {
            ui_show_ota(version, pct);
            last_pct = pct;
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        goto fail;
    }
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image invalid: %s", esp_err_to_name(err));
        goto fail;
    }
    nvs_handle_t nv;
    if (nvs_open("dev", NVS_READWRITE, &nv) == ESP_OK) {
        nvs_erase_key(nv, "ota_fail");
        nvs_commit(nv);
        nvs_close(nv);
    }
    ui_show_ota(version, 100);
    ESP_LOGI(TAG, "update installed, restarting");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();

fail:
    ui_show_ota(version, -1);
    return false;
}
