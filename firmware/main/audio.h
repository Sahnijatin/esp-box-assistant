#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void audio_init(void);
void audio_set_volume(int percent);

// Records 16 kHz mono 16-bit audio into buf until the speaker stops talking
// (about 1 s of silence), *stop becomes true, or max_samples is reached.
// level_cb (optional) gets 0-100 for a level meter. Returns samples recorded,
// or 0 if no speech was heard.
size_t audio_record(int16_t *buf, size_t max_samples, volatile bool *stop, void (*level_cb)(int));

// Streaming playback of 16-bit mono PCM
void audio_play_begin(int sample_rate);
void audio_play_write(const uint8_t *data, size_t len);
void audio_play_end(void);

// Short beeps for feedback
void audio_beep(int freq_hz, int ms);

// 44-byte WAV header for 16-bit mono PCM
void audio_wav_header(uint8_t *out, size_t samples, int sample_rate);
