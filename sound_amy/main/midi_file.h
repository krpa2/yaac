#pragma once

/* Minimal Standard MIDI File reader (format 0/1, any number of tracks).
 * Portable C, no AMY or ESP-IDF dependencies. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t ms;        /* absolute time from song start */
    uint8_t  status;    /* 0x80 note off, 0x90 note on, 0xB0 CC, 0xC0 program, 0xE0 bend (| channel) */
    uint8_t  d1;
    uint8_t  d2;
} midi_event_t;

typedef struct {
    midi_event_t *events;           /* time-sorted, channel messages only */
    size_t        count;
    uint32_t      length_ms;
    uint16_t      notes[16];        /* note-ons per channel */
    uint8_t       max_poly[16];     /* most notes held at once, per channel */
    int16_t       first_program[16];/* -1 if the channel never sends a program change */
} midi_song_t;

/* Parses data into song (allocates song->events). Returns 0 on success. */
int  midi_file_load(const uint8_t *data, size_t len, midi_song_t *song);
void midi_file_free(midi_song_t *song);
