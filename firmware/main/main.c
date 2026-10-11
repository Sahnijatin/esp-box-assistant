// ESP Box Assistant: tap-to-talk AI Q&A for the ESP32-S3-BOX-3.
// The box is a thin client. AI provider, API keys, Wi-Fi networks and firmware
// updates are all managed from the setup webpage through the backend.

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "app_config.h"
#include "ui.h"
#include "audio.h"
#include "wifi_mgr.h"
#include "cloud.h"
#include "assistant.h"
#include "improv.h"

static const char *TAG = "main";

static void on_wifi_submit(const char *ssid, const char *pw) { wifi_mgr_add_local(ssid, pw); }

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    improv_start();   // first, so the web installer finds it right after flashing

    const char *ver = esp_app_get_description()->version;
    ESP_LOGI(TAG, "ESP Box Assistant v%s", ver);

    bsp_i2c_init();
    bsp_display_start();
    bsp_display_brightness_set(DEFAULT_BRIGHTNESS);

    ui_init();
    ui_set_version(ver);
    ui_set_mode(UI_BLOCKED, "Starting...");
    ui_on_talk(assistant_tap);
    ui_on_wifi_submit(on_wifi_submit);
    ui_on_wifi_rescan(wifi_mgr_rescan);
    ui_on_wifi_open(wifi_mgr_open_setup);
    ui_on_wifi_close(wifi_mgr_close_setup);

    audio_init();
    assistant_start();
    wifi_mgr_init();
    cloud_start();
}
