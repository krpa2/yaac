#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REST 0

typedef struct {
    uint8_t note;   /* MIDI note number, REST for silence */
    float   beats;  /* length in quarter-note beats */
} score_step_t;

typedef struct {
    const score_step_t *steps;
    size_t count;
    float velocity;
    float gate;     /* fraction of the step the key is held */
} score_track_t;

extern const float imperial_march_bpm;
extern const score_track_t imperial_march_tracks[];
extern const size_t imperial_march_track_count;

#ifdef __cplusplus
}
#endif
