/**
 * wake_word.h — local "Computer" wake word (esp-sr WakeNet9, model partition).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the wn9 model from the "model" partition; returns 0 on success
   (or false from wake_word_ready() if it failed — app falls back to
   hands-free without a wake gate). */
int wake_word_init(void);
bool wake_word_ready(void);
const char *wake_word_name(void);

/* Feeds one frame of mono 16 kHz PCM (as captured by the mic task).
   Detection runs only while armed; posting APP_EVENT_WAKE_WORD re-arms
   nothing — the app decides when to disarm/re-arm. */
void wake_word_set_armed(bool armed);
void wake_word_feed(const int16_t *pcm, int nsamples);

#ifdef __cplusplus
}
#endif