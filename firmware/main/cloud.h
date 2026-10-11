#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Talks to the backend: check-in (pairing + config + updates), ask, text-to-speech.
void cloud_start(void);
bool cloud_paired(void);
void cloud_poll_now(void);

// Sends a WAV recording. On success *question / *answer / *speech are malloc'd (caller frees);
// *speech is the text to speak (can differ from the on-screen answer, e.g. Hinglish).
// *render is true when the text needs server rendering (Hindi etc.).
// On failure returns false and *error is malloc'd with a short message.
bool cloud_ask(const uint8_t *wav, size_t len, char **question, char **answer, char **speech,
               bool *render, char **error);

// Gets question + answer drawn by the server as an RGB565 bitmap (PSRAM, caller owns it).
bool cloud_render(const char *question, const char *answer, uint8_t **rgb565, int *w, int *h);

// Streams the spoken answer to the speaker. Stops early if *stop becomes true.
bool cloud_speak(const char *text, volatile bool *stop);
