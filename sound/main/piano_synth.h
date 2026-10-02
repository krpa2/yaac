#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_SAMPLE_RATE   44100
#define SYNTH_MAX_VOICES    12

/* Simple additive "synth piano": a few slightly inharmonic partials with
 * per-partial exponential decay, a detuned second string and a short release. */
void piano_synth_init(void);
void piano_note_on(uint8_t midi_note, float velocity);
void piano_note_off(uint8_t midi_note);

/* Render interleaved stereo int16 frames (L == R). */
void piano_render(int16_t *out, size_t frames);

#ifdef __cplusplus
}
#endif
