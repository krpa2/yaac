/*
 * "The Imperial March" (John Williams) — main theme, G minor, arranged for
 * piano: right-hand melody plus a left-hand bass on every beat.
 */
#include "imperial_march.h"

/* MIDI note numbers */
enum {
    /* bass is kept in octave 3: the small onboard speaker cannot reproduce ~100 Hz */
    D3 = 50, Eb3 = 51, G3 = 55,
    Eb4 = 63, E4 = 64, F4 = 65, Fs4 = 66, G4 = 67, Gs4 = 68, A4 = 69, Bb4 = 70, B4 = 71,
    C5 = 72, Cs5 = 73, D5 = 74, Eb5 = 75, E5 = 76, F5 = 77, Fs5 = 78, G5 = 79,
};

#define Q   1.0f    /* quarter */
#define H   2.0f    /* half */
#define E   0.5f    /* eighth */
#define DE  0.75f   /* dotted eighth */
#define S   0.25f   /* sixteenth */

const float imperial_march_bpm = 104.0f;

static const score_step_t melody[] = {
    /* bars 1-2 */
    {G4, Q}, {G4, Q}, {G4, Q}, {Eb4, DE}, {Bb4, S},
    {G4, Q}, {Eb4, DE}, {Bb4, S}, {G4, H},
    /* bars 3-4 */
    {D5, Q}, {D5, Q}, {D5, Q}, {Eb5, DE}, {Bb4, S},
    {Fs4, Q}, {Eb4, DE}, {Bb4, S}, {G4, H},
    /* bars 5-6 */
    {G5, Q}, {G4, DE}, {G4, S}, {G5, Q}, {Fs5, DE}, {F5, S},
    {E5, S}, {Eb5, S}, {E5, E}, {REST, E}, {Gs4, E}, {Cs5, Q}, {C5, DE}, {B4, S},
    /* bars 7-8 (first ending) */
    {Bb4, S}, {A4, S}, {Bb4, E}, {REST, E}, {Eb4, E}, {Fs4, Q}, {Eb4, DE}, {G4, S},
    {Bb4, Q}, {G4, DE}, {Bb4, S}, {D5, H},
    /* bars 9-10 */
    {G5, Q}, {G4, DE}, {G4, S}, {G5, Q}, {Fs5, DE}, {F5, S},
    {E5, S}, {Eb5, S}, {E5, E}, {REST, E}, {Gs4, E}, {Cs5, Q}, {C5, DE}, {B4, S},
    /* bars 11-12 (second ending) */
    {Bb4, S}, {A4, S}, {Bb4, E}, {REST, E}, {Eb4, E}, {Fs4, Q}, {Eb4, DE}, {Bb4, S},
    {G4, Q}, {Eb4, DE}, {Bb4, S}, {G4, H},
    /* breath before the loop restarts */
    {REST, H},
};

static const score_step_t bass[] = {
    /* bars 1-4: G pedal */
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    /* bars 5-8 */
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    {G3, Q}, {G3, Q}, {Eb3, Q}, {Eb3, Q},
    {G3, Q}, {G3, Q}, {D3, H},
    /* bars 9-12 */
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    {G3, Q}, {G3, Q}, {G3, Q}, {G3, Q},
    {G3, Q}, {G3, Q}, {Eb3, Q}, {Eb3, Q},
    {G3, Q}, {Eb3, Q}, {G3, H},
    {REST, H},
};

const score_track_t imperial_march_tracks[] = {
    {melody, sizeof(melody) / sizeof(melody[0]), 0.90f, 0.90f},
    {bass,   sizeof(bass) / sizeof(bass[0]),     0.55f, 0.80f},
};
const size_t imperial_march_track_count = sizeof(imperial_march_tracks) / sizeof(imperial_march_tracks[0]);
