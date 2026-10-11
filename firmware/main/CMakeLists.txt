idf_component_register(
    SRCS "main.c" "ui.c" "wifi_mgr.c" "portal.c" "cloud.c" "audio.c" "ota.c" "assistant.c" "improv.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client esp_https_ota
             esp_http_server app_update json mbedtls esp_timer lwip esp_app_format
             esp_driver_usb_serial_jtag
)
