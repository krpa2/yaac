#pragma once

/* Portable score player for AMY (no ESP-IDF dependencies): shared by the
 * firmware and pc_sim. Songs describe AMY synths plus note tracks; the player
 * turns tracks into timestamped AMY note events a little ahead of the clock. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "imperial_march.h"     /* score_step_t, REST */

#define PLAYER_MAX_TRACKS 8

typedef struct {
    const score_step_t *steps;
    size_t  count;
    uint8_t synth;      /* AMY synth that plays this track */
    float   velocity;
    float   gate;       /* fraction of each step the note is held */
} player_track_t;

typedef struct {
    const char *name;
    float       bpm;
    uint16_t    max_oscs;       /* AMY osc pool this song needs (sets core split) */
    float       volume;         /* AMY master volume for this song */
    void      (*setup)(void);   /* define the song's AMY synths / effects */
    const player_track_t *tracks;
    size_t      track_count;
} amy_song_t;

typedef struct {
    size_t  index;
    double  next_on_ms;
    double  off_ms;
    uint8_t held_note;
} player_track_pos_t;

typedef struct {
    const amy_song_t  *song;
    double             ms_per_beat;
    player_track_pos_t pos[PLAYER_MAX_TRACKS];
} player_t;

void player_start(player_t *p, const amy_song_t *song, uint32_t start_ms);

/* Queue every note-on/off at or before horizon_ms (AMY applies each at its own
 * timestamp). Returns false once the whole song has been queued. */
bool player_feed(player_t *p, uint32_t horizon_ms);

/* Songs */
const amy_song_t *song_korobeiniki(void);
const amy_song_t *song_imperial_march_piano(void);
