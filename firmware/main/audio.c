#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "audio.h"
#include "app_config.h"

static const char *TAG = "audio";
static esp_codec_dev_handle_t spk, mic;
static int volume = DEFAULT_VOLUME;
static SemaphoreHandle_t lock;   // mic and speaker share one I2S bus: one at a time

#define FRAME 320   // 20 ms at 16 kHz

void audio_init(void)
{
    lock = xSemaphoreCreateMutex();
    spk = bsp_audio_codec_speaker_init();
    mic = bsp_audio_codec_microphone_init();
    if (!spk || !mic) ESP_LOGE(TAG, "codec init failed (spk=%p mic=%p)", spk, mic);
    if (spk) esp_codec_dev_set_out_vol(spk, volume);
    if (mic) esp_codec_dev_set_in_gain(mic, 40.0);
}

void audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    volume = percent;
    if (spk) esp_codec_dev_set_out_vol(spk, volume);
}

static int frame_rms(const int16_t *s, int n)
{
    int64_t acc = 0;
    for (int i = 0; i < n; i++) acc += (int32_t)s[i] * s[i];
    return (int)sqrtf((float)acc / n);
}

size_t audio_record(int16_t *buf, size_t max_samples, volatile bool *stop, void (*level_cb)(int))
{
    if (!mic) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = REC_SAMPLE_RATE, .channel = 1, .bits_per_sample = 16,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    esp_codec_dev_open(mic, &fs);

    size_t n = 0;
    int noise = 0, frames = 0, loud_run = 0, silent = 0;
    bool speech = false;
    const int calib_frames = 10;              // first 200 ms: measure room noise
    const int silence_end = 55;               // 1.1 s of quiet ends the question
    const int no_speech_timeout = 6000 / 20;  // give up after 6 s of nothing

    while (n + FRAME <= max_samples) {
        if (esp_codec_dev_read(mic, buf + n, FRAME * sizeof(int16_t)) != ESP_CODEC_DEV_OK) break;
        int rms = frame_rms(buf + n, FRAME);
        n += FRAME;
        frames++;
        if (level_cb && (frames % 3) == 0) {
            int lvl = rms / 40;
            level_cb(lvl > 100 ? 100 : lvl);
        }
        if (frames <= calib_frames) {
            noise += rms;
            if (frames == calib_frames) noise = noise / calib_frames;
            continue;
        }
        int threshold = noise * 3;
        if (threshold < 300) threshold = 300;
        if (rms > threshold) {
            silent = 0;
            if (++loud_run >= 3) speech = true;
        } else {
            loud_run = 0;
            if (speech && rms < threshold * 7 / 10) silent++;
        }
        if (stop && *stop) break;
        if (speech && silent >= silence_end) break;
        if (!speech && frames >= no_speech_timeout) break;
    }
    esp_codec_dev_close(mic);
    xSemaphoreGive(lock);

    bool user_stopped = stop && *stop;
    if (!speech && !(user_stopped && n > REC_SAMPLE_RATE / 2)) {
        ESP_LOGI(TAG, "no speech detected");
        return 0;
    }
    ESP_LOGI(TAG, "recorded %.1f s (noise %d)", (float)n / REC_SAMPLE_RATE, noise);
    return n;
}

void audio_play_begin(int sample_rate)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_codec_dev_sample_info_t fs = { .sample_rate = sample_rate, .channel = 1, .bits_per_sample = 16 };
    if (spk) {
        esp_codec_dev_open(spk, &fs);
        esp_codec_dev_set_out_vol(spk, volume);
    }
}

void audio_play_write(const uint8_t *data, size_t len)
{
    if (spk && len) esp_codec_dev_write(spk, (void *)data, len);
}

void audio_play_end(void)
{
    if (spk) {
        // flush a little silence so the last word isn't clipped
        static const uint8_t zeros[1024] = { 0 };
        esp_codec_dev_write(spk, (void *)zeros, sizeof(zeros));
        esp_codec_dev_close(spk);
    }
    xSemaphoreGive(lock);
}

void audio_beep(int freq_hz, int ms)
{
    const int rate = 16000;
    int total = rate * ms / 1000;
    int16_t chunk[256];
    audio_play_begin(rate);
    for (int i = 0; i < total;) {
        int c = total - i < 256 ? total - i : 256;
        for (int k = 0; k < c; k++, i++) {
            float env = i < 160 ? i / 160.0f : (total - i < 160 ? (total - i) / 160.0f : 1.0f);
            chunk[k] = (int16_t)(sinf(2 * (float)M_PI * freq_hz * i / rate) * 6000 * env);
        }
        audio_play_write((uint8_t *)chunk, c * sizeof(int16_t));
    }
    audio_play_end();
}

void audio_wav_header(uint8_t *h, size_t samples, int rate)
{
    uint32_t data = samples * 2, riff = data + 36, byte_rate = rate * 2;
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
}
