#pragma once

/* Post-processing of the AFE output (after WakeNet, so it does not affect wake
 * word detection): WebRTC noise suppression + WebRTC automatic gain control,
 * using ESP-SR's standalone esp_ns / esp_agc. Both work on 10 ms frames, so
 * input is re-blocked through a small FIFO (10 ms extra latency). */
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_POST_FRAME        160     /* 10 ms at 16 kHz */

typedef struct {
    int  ns_mode;               /* -1 off, 0 mild, 1 medium, 2 aggressive */
    bool agc;                   /* WebRTC AGC, adaptive digital mode */
    int  agc_target_level_dbfs; /* positive: 3 means -3 dBFS */
    int  agc_compression_gain_db;
} audio_post_config_t;

void audio_post_init(const audio_post_config_t *cfg);

/* Process n samples; writes the processed samples available so far to out
 * (room for n + AUDIO_POST_FRAME samples) and returns how many were written */
int audio_post_process(const int16_t *in, int n, int16_t *out);
