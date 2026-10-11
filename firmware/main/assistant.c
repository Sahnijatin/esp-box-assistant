#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app_config.h"
#include "assistant.h"
#include "audio.h"
#include "cloud.h"
#include "ui.h"
#include "wifi_mgr.h"

static const char *TAG = "assistant";

typedef enum { ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING } state_t;

static volatile state_t state = ST_IDLE;
static volatile bool stop_flag;
static QueueHandle_t taps;
static uint8_t *rec;      // WAV header + samples, in PSRAM
#define REC_SAMPLES (REC_SAMPLE_RATE * REC_MAX_SECONDS)

bool assistant_busy(void) { return state != ST_IDLE; }

void assistant_tap(void)
{
    if (state == ST_LISTENING || state == ST_SPEAKING) {
        stop_flag = true;          // tap again = stop
        return;
    }
    if (state == ST_IDLE) {
        uint8_t x = 1;
        xQueueSend(taps, &x, 0);
    }
}

static void level(int pct) { ui_set_level(pct); }

static void handle_question(void)
{
    if (!wifi_mgr_connected()) {
        ui_set_mode(UI_BLOCKED, "No Wi-Fi. Tap the top bar to set it up.");
        return;
    }
    if (!cloud_paired()) {
        ui_set_mode(UI_BLOCKED, "Pair this box from the setup page first");
        return;
    }

    // 1. listen
    state = ST_LISTENING;
    stop_flag = false;
    ui_set_mode(UI_LISTENING, "Ask your question");
    ui_show_question("");
    ui_show_answer("");
    audio_beep(880, 80);
    size_t n = audio_record((int16_t *)(rec + 44), REC_SAMPLES, &stop_flag, level);
    if (n == 0) {
        state = ST_IDLE;
        ui_set_mode(UI_IDLE, "I didn't hear anything. Try again.");
        return;
    }
    audio_beep(660, 60);

    // 2. think
    state = ST_THINKING;
    ui_set_mode(UI_THINKING, NULL);
    audio_wav_header(rec, n, REC_SAMPLE_RATE);
    char *q = NULL, *a = NULL, *sp = NULL, *err = NULL;
    bool render = false;
    bool ok = cloud_ask(rec, 44 + n * 2, &q, &a, &sp, &render, &err);
    if (!ok) {
        ESP_LOGW(TAG, "ask failed: %s", err ? err : "?");
        ui_show_question(q);
        ui_show_answer(err ? err : "Something went wrong.");
        state = ST_IDLE;
        ui_set_mode(UI_IDLE, NULL);
        free(q); free(a); free(sp); free(err);
        return;
    }

    // 3. show: Hindi and other scripts come back as a server-drawn image
    uint8_t *img = NULL;
    int iw = 0, ih = 0;
    if (render && cloud_render(q, a, &img, &iw, &ih)) {
        ui_show_rendered(img, iw, ih);
    } else {
        ui_show_question(q);
        ui_show_answer(a);
    }

    // 4. speak
    state = ST_SPEAKING;
    stop_flag = false;
    ui_set_mode(UI_SPEAKING, NULL);
    if (!cloud_speak(sp, &stop_flag)) ui_set_mode(UI_IDLE, "Couldn't play the answer");
    else ui_set_mode(UI_IDLE, NULL);
    state = ST_IDLE;
    free(q); free(a); free(sp); free(err);
}

static void assistant_task(void *arg)
{
    uint8_t x;
    for (;;) {
        if (xQueueReceive(taps, &x, portMAX_DELAY)) {
            handle_question();
            xQueueReset(taps);    // ignore taps made while busy
        }
    }
}

void assistant_start(void)
{
    rec = heap_caps_malloc(44 + REC_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    taps = xQueueCreate(2, 1);
    xTaskCreatePinnedToCore(assistant_task, "assistant", 8192, NULL, 6, NULL, 1);
}
