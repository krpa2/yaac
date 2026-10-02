#pragma once

/* MIDI files embedded from the ../assets folder (table generated at build time) */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char    *name;
    const uint8_t *start;
    const uint8_t *end;
} midi_playlist_entry_t;

extern const midi_playlist_entry_t midi_playlist[];
extern const size_t midi_playlist_count;
