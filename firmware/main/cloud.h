#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Talks to the backend: check-in (pairing + config + updates), ask, text-to-speech.
void cloud_start(void);
bool cloud_paired(void);
void cloud_poll_now(void);

// Sends a WAV recording. On success *question / *answer are malloc'd (caller frees).
// On failure returns false and *error is malloc'd with a short message.
bool cloud_ask(const uint8_t *wav, size_t len, char **question, char **answer, char **error);

// Streams the spoken answer to the speaker. Stops early if *stop becomes true.
bool cloud_speak(const char *text, volatile bool *stop);
