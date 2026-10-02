#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "imperial_march.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SEQ_MAX_TRACKS  4

/* Portable (no ESP-IDF dependencies): shared by the firmware and pc_sim. */
typedef struct {
    size_t   index;
    uint64_t next_on;       /* sample at which the next step starts */
    uint64_t pending_off;   /* sample at which the held key is released */
    uint8_t  held_note;
} seq_track_cursor_t;

typedef struct {
    const score_track_t *tracks;
    size_t   track_count;
    float    samples_per_beat;
    uint64_t now;
    seq_track_cursor_t cur[SEQ_MAX_TRACKS];
} sequencer_t;

void sequencer_start(sequencer_t *seq, const score_track_t *tracks, size_t track_count, float bpm);

/* Render interleaved stereo int16 frames with sample-accurate note timing.
 * Returns false once every track has finished (release tails keep ringing
 * in the synth and are rendered by subsequent piano_render calls). */
bool sequencer_render(sequencer_t *seq, int16_t *out, size_t frames);

#ifdef __cplusplus
}
#endif
