#pragma once

/* Speaker output: a PSRAM ring buffer drained by one player task into the
 * ES8311 (audio_play), so the chime and Gemini's voice never write to I2S at
 * the same time. Gemini replies are 24 kHz and get resampled to the 16 kHz the
 * shared I2S link runs at. The ES7210's echo-reference input hears everything
 * played here, so the AFE's AEC removes it from the microphones. */
#include <stdbool.h>
#include <stdint.h>

void playback_init(void);

/* Queue 16 kHz mono samples (any task; drops what does not fit) */
void playback_push(const int16_t *pcm, int n);

/* Queue 24 kHz mono samples (Gemini reply audio); resampled to 16 kHz */
void playback_push_24k(const int16_t *pcm, int n);

/* Drop everything queued (barge-in) */
void playback_clear(void);

/* True while queued audio is still playing */
bool playback_active(void);
