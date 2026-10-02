/* The Imperial March (score shared with ../sound) on AMY's built-in piano. */
#include "amy.h"
#include "amy_player.h"

#define PIANO_SYNTH     1
#define PIANO_PATCH     256     /* AMY built-in partials piano */
#define PIANO_VOICES    4       /* melody + bass + their release tails */
#define OSCS_PER_VOICE  25      /* patch 256: 1 control osc + 24 partials */

static void setup(void)
{
    amy_event e = amy_default_event();
    e.synth = PIANO_SYNTH;
    e.patch_number = PIANO_PATCH;
    e.num_voices = PIANO_VOICES;
    amy_add_event(&e);
}

const amy_song_t *song_imperial_march_piano(void)
{
    static player_track_t tracks[PLAYER_MAX_TRACKS];
    static amy_song_t song = {
        .name = "The Imperial March (AMY piano, patch 256)",
        /* AMY splits oscs by index across the two cores, so size the pool to
         * the voices in use; partials also need a little headroom (AMY asserts). */
        .max_oscs = PIANO_VOICES * OSCS_PER_VOICE + 4,
        .volume = 1.0f,
        .setup = setup,
        .tracks = tracks,
    };
    song.bpm = imperial_march_bpm;
    song.track_count = imperial_march_track_count;
    for (size_t t = 0; t < imperial_march_track_count && t < PLAYER_MAX_TRACKS; t++) {
        tracks[t] = (player_track_t){
            .steps = imperial_march_tracks[t].steps,
            .count = imperial_march_tracks[t].count,
            .synth = PIANO_SYNTH,
            .velocity = imperial_march_tracks[t].velocity,
            .gate = imperial_march_tracks[t].gate,
        };
    }
    return &song;
}
