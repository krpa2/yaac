#pragma once

/* Full-duplex board audio on I2S0 (TDM, 4 slots, 16 kHz, 16 bit):
 *   in : ES7210 -> slot 0 MIC1, slot 1 speaker echo reference, slot 2 MIC2, slot 3 unused
 *   out: ES8311 -> speaker amp (TCA9554 EXIO7)
 * Both directions share BCLK/WS, so they use the same TDM layout. */
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_IN_CHANNELS   4
#define AUDIO_IN_FORMAT     "MRMN"      /* ESP-SR AFE channel layout of the input frames */

void audio_io_init(i2c_master_bus_handle_t i2c_bus);

/* Read frames x AUDIO_IN_CHANNELS interleaved samples (blocking) */
int  audio_read(int16_t *buf, size_t frames);

/* Play mono samples on the speaker (blocking until queued) */
void audio_play(const int16_t *mono, size_t n);

void audio_mute(void);
