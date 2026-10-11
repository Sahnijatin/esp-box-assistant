#pragma once
#include <stdbool.h>
#include "esp_wifi_types.h"

typedef enum {
    UI_IDLE,
    UI_LISTENING,
    UI_THINKING,
    UI_SPEAKING,
    UI_BLOCKED,      // can't talk right now (no Wi-Fi / not paired); hint says why
} ui_mode_t;

void ui_init(void);

// Top bar
void ui_set_wifi(bool connected, const char *ssid);
void ui_set_version(const char *version);

// Main screen
void ui_set_mode(ui_mode_t mode, const char *hint);
void ui_set_level(int percent);              // mic level while listening
void ui_show_question(const char *text);
void ui_show_answer(const char *text);
void ui_set_brightness(int percent);

// Overlays
void ui_show_pairing(const char *code);
void ui_hide_pairing(void);
void ui_show_ota(const char *version, int percent);   // percent < 0 hides it

// Wi-Fi setup screen
void ui_wifi_show(const char *ap_name, bool can_close);
void ui_wifi_hide(void);
bool ui_wifi_visible(void);
void ui_wifi_set_results(const wifi_ap_record_t *aps, int count);
void ui_wifi_set_message(const char *msg);

// Callbacks (called from the LVGL task: keep them short, post to a queue)
void ui_on_talk(void (*cb)(void));
void ui_on_wifi_submit(void (*cb)(const char *ssid, const char *password));
void ui_on_wifi_rescan(void (*cb)(void));
void ui_on_wifi_open(void (*cb)(void));
void ui_on_wifi_close(void (*cb)(void));
