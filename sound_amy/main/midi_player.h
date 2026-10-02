#pragma once

/* Plays a parsed MIDI file on AMY: one AMY synth per MIDI channel (synth = channel + 1),
 * General MIDI programs mapped to AMY's Juno-6 / DX7 patches, channel 10 on AMY's
 * GM drum kit. Portable (AMY only): shared by the firmware and pc_sim. */
#include <stdbool.h>
#include <stdint.h>
#include "amy.h"
#include "midi_file.h"

#define MIDI_MAX_VOICES_PER_CHANNEL 6   /* polyphony cap per channel (CPU) */
#define MIDI_VOLUME 2.0f                /* AMY master volume for MIDI files (GM velocities run quiet) */

typedef struct {
    const midi_song_t *song;
    size_t   index;
    uint32_t start_ms;
    float    volume[16];        /* CC7 */
    float    expression[16];    /* CC11 */
    uint16_t patch[16];         /* AMY patch currently on each channel's synth */
} midi_player_t;

/* AMY patch for a General MIDI program (0-127) on a melodic channel */
uint16_t midi_gm_to_amy_patch(uint8_t program);
/* Name of the raw-oscillator chip voice used for this GM program, or NULL if it
 * gets an AMY patch (midi_gm_to_amy_patch) instead */
const char *midi_gm_chip_voice(uint8_t program);

/* Define AMY synths for every channel the song uses */
void midi_player_setup(midi_player_t *p, const midi_song_t *song);
/* AMY oscs occupied by those synths (once AMY has executed the setup events) */
int  midi_player_count_oscs(const midi_song_t *song);

/* Start AMY for this song with an osc pool sized to fit: AMY splits rendering
 * by osc index across cores, so an oversized pool leaves one core idle. Starts
 * AMY once to measure what the patches occupy, then restarts it sized to that.
 * Call from the task that will call amy_update(). Returns the osc pool size. */
int  midi_player_amy_start(midi_player_t *p, const midi_song_t *song, const amy_config_t *config, float volume);

void midi_player_start(midi_player_t *p, uint32_t start_ms);

/* Queue every MIDI event due at or before horizon_ms as timestamped AMY events.
 * Returns false once the whole file has been queued. */
bool midi_player_feed(midi_player_t *p, uint32_t horizon_ms);
