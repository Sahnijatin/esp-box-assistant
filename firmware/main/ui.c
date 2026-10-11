// LVGL user interface for the 320x240 touchscreen.
// Every public function takes the display lock, so they can be called from any task.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "ui.h"

#define C_BG      0x0f1115
#define C_CARD    0x1b1f27
#define C_TEXT    0xe8eaf0
#define C_MUTED   0x9aa3b2
#define C_BLUE    0x3b82f6
#define C_RED     0xef4444
#define C_AMBER   0xf59e0b
#define C_GREEN   0x10b981
#define C_GRAY    0x4b5563

static lv_obj_t *wifi_lbl, *ver_lbl;
static lv_obj_t *chat, *q_lbl, *a_lbl, *r_img;
static lv_image_dsc_t r_dsc;
static uint8_t *r_buf;
static lv_obj_t *talk_btn, *talk_lbl, *level_bar, *hint_lbl;
static lv_obj_t *pair_panel, *pair_code_lbl;
static lv_obj_t *ota_panel, *ota_lbl, *ota_bar;
static lv_obj_t *wifi_panel, *wifi_list, *wifi_msg, *wifi_close_btn;
static lv_obj_t *pw_panel, *pw_title, *pw_ta, *pw_kb;
static char selected_ssid[33];

static void (*talk_cb)(void);
static void (*wifi_submit_cb)(const char *, const char *);
static void (*wifi_rescan_cb)(void);
static void (*wifi_open_cb)(void);
static void (*wifi_close_cb)(void);

void ui_on_talk(void (*cb)(void)) { talk_cb = cb; }
void ui_on_wifi_submit(void (*cb)(const char *, const char *)) { wifi_submit_cb = cb; }
void ui_on_wifi_rescan(void (*cb)(void)) { wifi_rescan_cb = cb; }
void ui_on_wifi_open(void (*cb)(void)) { wifi_open_cb = cb; }
void ui_on_wifi_close(void (*cb)(void)) { wifi_close_cb = cb; }

#define LOCK()   bsp_display_lock(0)
#define UNLOCK() bsp_display_unlock()

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    return p;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *txt, uint32_t color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_height(b, 32);
    lv_obj_t *l = label(b, &lv_font_montserrat_14, C_TEXT, txt);
    lv_obj_center(l);
    return b;
}

// ---------------- events ----------------

static void on_talk(lv_event_t *e) { if (talk_cb) talk_cb(); }
static void on_wifi_icon(lv_event_t *e) { if (wifi_open_cb) wifi_open_cb(); }
static void on_rescan(lv_event_t *e)
{
    lv_label_set_text(wifi_msg, "Scanning...");
    if (wifi_rescan_cb) wifi_rescan_cb();
}
static void on_wifi_close(lv_event_t *e) { if (wifi_close_cb) wifi_close_cb(); }

static void on_ssid_selected(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    const char *ssid = lv_list_get_button_text(wifi_list, btn);
    if (!ssid) return;
    strlcpy(selected_ssid, ssid, sizeof(selected_ssid));
    lv_label_set_text_fmt(pw_title, "Password for %s", selected_ssid);
    lv_textarea_set_text(pw_ta, "");
    lv_obj_clear_flag(pw_panel, LV_OBJ_FLAG_HIDDEN);
}

static void on_kb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *pw = lv_textarea_get_text(pw_ta);
        lv_obj_add_flag(pw_panel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(wifi_msg, "Connecting to %s...", selected_ssid);
        if (wifi_submit_cb) wifi_submit_cb(selected_ssid, pw);
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(pw_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------- build ----------------

static void build_main(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // top bar
    lv_obj_t *bar = panel(scr, 0, 0, 320, 26, C_CARD);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, on_wifi_icon, LV_EVENT_CLICKED, NULL);
    wifi_lbl = label(bar, &lv_font_montserrat_14, C_MUTED, LV_SYMBOL_WIFI "  Connecting...");
    lv_obj_align(wifi_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    ver_lbl = label(bar, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_align(ver_lbl, LV_ALIGN_RIGHT_MID, -8, 0);

    // chat area
    chat = lv_obj_create(scr);
    lv_obj_set_pos(chat, 6, 30);
    lv_obj_set_size(chat, 308, 140);
    lv_obj_set_style_bg_opa(chat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat, 0, 0);
    lv_obj_set_style_pad_all(chat, 4, 0);
    lv_obj_set_style_pad_row(chat, 8, 0);
    lv_obj_set_flex_flow(chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(chat, LV_SCROLLBAR_MODE_AUTO);

    q_lbl = label(chat, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_set_width(q_lbl, lv_pct(100));
    lv_label_set_long_mode(q_lbl, LV_LABEL_LONG_WRAP);
    a_lbl = label(chat, &lv_font_montserrat_16, C_TEXT, "Hi! Tap the button and ask me anything.");
    lv_obj_set_width(a_lbl, lv_pct(100));
    lv_label_set_long_mode(a_lbl, LV_LABEL_LONG_WRAP);
    r_img = lv_image_create(chat);
    lv_obj_add_flag(r_img, LV_OBJ_FLAG_HIDDEN);

    // hint + level
    hint_lbl = label(scr, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -66);
    level_bar = lv_bar_create(scr);
    lv_obj_set_size(level_bar, 300, 4);
    lv_obj_align(level_bar, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(C_RED), LV_PART_INDICATOR);
    lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);

    // talk button
    talk_btn = lv_button_create(scr);
    lv_obj_set_size(talk_btn, 308, 52);
    lv_obj_align(talk_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(talk_btn, 26, 0);
    lv_obj_set_style_bg_color(talk_btn, lv_color_hex(C_BLUE), 0);
    lv_obj_add_event_cb(talk_btn, on_talk, LV_EVENT_CLICKED, NULL);
    talk_lbl = label(talk_btn, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_AUDIO "  Tap to ask");
    lv_obj_center(talk_lbl);
}

static void build_pairing(lv_obj_t *top)
{
    pair_panel = panel(top, 0, 0, 320, 240, C_BG);
    lv_obj_t *t = label(pair_panel, &lv_font_montserrat_20, C_TEXT, "Pair this box");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);
    pair_code_lbl = label(pair_panel, &lv_font_montserrat_40, C_BLUE, "------");
    lv_obj_set_style_text_letter_space(pair_code_lbl, 6, 0);
    lv_obj_align(pair_code_lbl, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t *h = label(pair_panel, &lv_font_montserrat_14, C_MUTED,
                        "Open the setup page on your phone\nand enter this code under Boxes.");
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_add_flag(pair_panel, LV_OBJ_FLAG_HIDDEN);
}

static void build_ota(lv_obj_t *top)
{
    ota_panel = panel(top, 0, 0, 320, 240, C_BG);
    ota_lbl = label(ota_panel, &lv_font_montserrat_20, C_TEXT, "Updating...");
    lv_obj_align(ota_lbl, LV_ALIGN_CENTER, 0, -30);
    ota_bar = lv_bar_create(ota_panel);
    lv_obj_set_size(ota_bar, 260, 14);
    lv_obj_align(ota_bar, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(C_BLUE), LV_PART_INDICATOR);
    lv_obj_t *h = label(ota_panel, &lv_font_montserrat_14, C_MUTED, "Don't unplug the box.");
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 44);
    lv_obj_add_flag(ota_panel, LV_OBJ_FLAG_HIDDEN);
}

static void build_wifi(lv_obj_t *top)
{
    wifi_panel = panel(top, 0, 0, 320, 240, C_BG);
    lv_obj_clear_flag(wifi_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = label(wifi_panel, &lv_font_montserrat_16, C_TEXT, LV_SYMBOL_WIFI "  Wi-Fi setup");
    lv_obj_set_pos(t, 8, 8);

    wifi_close_btn = small_button(wifi_panel, LV_SYMBOL_CLOSE, C_GRAY);
    lv_obj_set_width(wifi_close_btn, 40);
    lv_obj_align(wifi_close_btn, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_event_cb(wifi_close_btn, on_wifi_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rescan = small_button(wifi_panel, LV_SYMBOL_REFRESH, C_GRAY);
    lv_obj_set_width(rescan, 40);
    lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, -52, 4);
    lv_obj_add_event_cb(rescan, on_rescan, LV_EVENT_CLICKED, NULL);

    wifi_msg = label(wifi_panel, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_set_width(wifi_msg, 304);
    lv_label_set_long_mode(wifi_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(wifi_msg, 8, 40);

    wifi_list = lv_list_create(wifi_panel);
    lv_obj_set_size(wifi_list, 308, 148);
    lv_obj_set_pos(wifi_list, 6, 88);
    lv_obj_set_style_bg_color(wifi_list, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(wifi_list, 0, 0);

    // password entry (full screen, over the list)
    pw_panel = panel(wifi_panel, 0, 0, 320, 240, C_BG);
    pw_title = label(pw_panel, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_pos(pw_title, 8, 6);
    pw_ta = lv_textarea_create(pw_panel);
    lv_textarea_set_one_line(pw_ta, true);
    lv_textarea_set_password_mode(pw_ta, false);   // visible: easier to type on a small screen
    lv_textarea_set_max_length(pw_ta, 63);
    lv_obj_set_size(pw_ta, 304, 40);
    lv_obj_set_pos(pw_ta, 8, 26);
    pw_kb = lv_keyboard_create(pw_panel);
    lv_obj_set_size(pw_kb, 320, 170);
    lv_obj_align(pw_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(pw_kb, pw_ta);
    lv_obj_add_event_cb(pw_kb, on_kb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(pw_kb, on_kb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(pw_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
}

void ui_init(void)
{
    LOCK();
    build_main(lv_screen_active());
    lv_obj_t *top = lv_layer_top();
    build_pairing(top);
    build_ota(top);
    build_wifi(top);
    UNLOCK();
}

// ---------------- public setters ----------------

void ui_set_wifi(bool connected, const char *ssid)
{
    LOCK();
    if (connected) {
        lv_label_set_text_fmt(wifi_lbl, LV_SYMBOL_WIFI "  %s", ssid ? ssid : "");
        lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(C_GREEN), 0);
    } else {
        lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI "  Not connected (tap)");
        lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(C_RED), 0);
    }
    UNLOCK();
}

void ui_set_version(const char *version)
{
    LOCK();
    lv_label_set_text_fmt(ver_lbl, "v%s", version);
    UNLOCK();
}

void ui_set_mode(ui_mode_t mode, const char *hint)
{
    LOCK();
    uint32_t color = C_BLUE;
    const char *txt = LV_SYMBOL_AUDIO "  Tap to ask";
    switch (mode) {
    case UI_LISTENING: color = C_RED;   txt = LV_SYMBOL_STOP "  Listening... tap to stop"; break;
    case UI_THINKING:  color = C_AMBER; txt = LV_SYMBOL_REFRESH "  Thinking..."; break;
    case UI_SPEAKING:  color = C_GREEN; txt = LV_SYMBOL_VOLUME_MAX "  Speaking... tap to stop"; break;
    case UI_BLOCKED:   color = C_GRAY;  break;
    default: break;
    }
    lv_obj_set_style_bg_color(talk_btn, lv_color_hex(color), 0);
    lv_label_set_text(talk_lbl, txt);
    lv_label_set_text(hint_lbl, hint ? hint : "");
    if (mode == UI_LISTENING) {
        lv_bar_set_value(level_bar, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
    }
    UNLOCK();
}

void ui_set_level(int percent)
{
    LOCK();
    lv_bar_set_value(level_bar, percent, LV_ANIM_OFF);
    UNLOCK();
}

// Back to text labels: drop any rendered bitmap
static void clear_rendered(void)
{
    if (!r_buf) return;
    lv_obj_add_flag(r_img, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(r_img, NULL);
    lv_image_cache_drop(&r_dsc);
    free(r_buf);
    r_buf = NULL;
    lv_obj_clear_flag(q_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(a_lbl, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_rendered(uint8_t *rgb565, int w, int h)
{
    LOCK();
    clear_rendered();
    r_buf = rgb565;
    memset(&r_dsc, 0, sizeof(r_dsc));
    r_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    r_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    r_dsc.header.w = w;
    r_dsc.header.h = h;
    r_dsc.header.stride = w * 2;
    r_dsc.data_size = w * h * 2;
    r_dsc.data = rgb565;
    lv_image_set_src(r_img, &r_dsc);
    lv_obj_add_flag(q_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(r_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(chat, 0, LV_ANIM_OFF);
    UNLOCK();
}

void ui_show_question(const char *text)
{
    LOCK();
    clear_rendered();
    if (text && *text) lv_label_set_text_fmt(q_lbl, "You: %s", text);
    else lv_label_set_text(q_lbl, "");
    UNLOCK();
}

void ui_show_answer(const char *text)
{
    LOCK();
    clear_rendered();
    lv_label_set_text(a_lbl, text ? text : "");
    lv_obj_scroll_to_y(chat, 0, LV_ANIM_OFF);
    UNLOCK();
}

void ui_set_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

void ui_show_pairing(const char *code)
{
    LOCK();
    lv_label_set_text(pair_code_lbl, code);
    lv_obj_clear_flag(pair_panel, LV_OBJ_FLAG_HIDDEN);
    UNLOCK();
}

void ui_hide_pairing(void)
{
    LOCK();
    lv_obj_add_flag(pair_panel, LV_OBJ_FLAG_HIDDEN);
    UNLOCK();
}

void ui_show_ota(const char *version, int percent)
{
    LOCK();
    if (percent < 0) {
        lv_obj_add_flag(ota_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text_fmt(ota_lbl, "Updating to v%s", version);
        lv_bar_set_value(ota_bar, percent, LV_ANIM_OFF);
        lv_obj_clear_flag(ota_panel, LV_OBJ_FLAG_HIDDEN);
    }
    UNLOCK();
}

void ui_wifi_show(const char *ap_name, bool can_close)
{
    LOCK();
    lv_label_set_text_fmt(wifi_msg,
        "Pick a network below, or join hotspot \"%s\" from your phone and open " "http://192.168.4.1",
        ap_name);
    if (can_close) lv_obj_clear_flag(wifi_close_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(wifi_close_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
    UNLOCK();
}

void ui_wifi_hide(void)
{
    LOCK();
    lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
    UNLOCK();
}

bool ui_wifi_visible(void)
{
    LOCK();
    bool v = !lv_obj_has_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
    UNLOCK();
    return v;
}

void ui_wifi_set_message(const char *msg)
{
    LOCK();
    lv_label_set_text(wifi_msg, msg);
    UNLOCK();
}

void ui_wifi_set_results(const wifi_ap_record_t *aps, int count)
{
    LOCK();
    lv_obj_clean(wifi_list);
    if (count == 0) lv_list_add_text(wifi_list, "No networks found. Tap refresh.");
    for (int i = 0; i < count; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(ssid, (const char *)aps[j].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        const char *icon = aps[i].authmode == WIFI_AUTH_OPEN ? LV_SYMBOL_WIFI : LV_SYMBOL_EYE_CLOSE;
        lv_obj_t *b = lv_list_add_button(wifi_list, icon, ssid);
        lv_obj_add_event_cb(b, on_ssid_selected, LV_EVENT_CLICKED, NULL);
    }
    UNLOCK();
}
