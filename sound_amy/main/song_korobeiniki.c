/*
 * "Korobeiniki" — the Russian folk song (public domain) best known as the
 * Tetris "Type A" theme — arranged as a 4-channel NES/Game Boy-style chiptune:
 *
 *   synth 1  lead     25% pulse, short decay, a touch of echo
 *   synth 2  arp      50% pulse, 16th-note chord arpeggios (the classic chip "fake chord")
 *   synth 3  bass     triangle, octave-jumping 8ths
 *   synth 4  kick     sine with a fast pitch drop
 *   synth 5  snare    noise, band-passed
 *   synth 6  hat      noise, high-passed, very short
 *
 * Form: A A B A. Only the melody is written out; bass, arp and drums are
 * generated from the per-bar chord list.
 */
#include <stddef.h>
#include "amy.h"
#include "amy_player.h"

enum { SYN_LEAD = 1, SYN_ARP, SYN_BASS, SYN_KICK, SYN_SNARE, SYN_HAT };

/* MIDI notes */
enum {
    Gs4 = 68, A4 = 69, B4 = 71, C5 = 72, D5 = 74, E5 = 76, F5 = 77, G5 = 79, Gs5 = 80, A5 = 81,
    KICK_NOTE = 45, SNARE_NOTE = 50, HAT_NOTE = 60,
};

#define Q   1.0f
#define H   2.0f
#define W   4.0f
#define E   0.5f
#define DQ  1.5f

static const score_step_t part_a[] = {
    {E5, Q}, {B4, E}, {C5, E}, {D5, Q}, {C5, E}, {B4, E},
    {A4, Q}, {A4, E}, {C5, E}, {E5, Q}, {D5, E}, {C5, E},
    {B4, DQ}, {C5, E}, {D5, Q}, {E5, Q},
    {C5, Q}, {A4, Q}, {A4, Q}, {REST, Q},
    {REST, E}, {D5, Q}, {F5, E}, {A5, Q}, {G5, E}, {F5, E},
    {E5, DQ}, {C5, E}, {E5, Q}, {D5, E}, {C5, E},
    {B4, Q}, {B4, E}, {C5, E}, {D5, Q}, {E5, Q},
    {C5, Q}, {A4, Q}, {A4, Q}, {REST, Q},
};

static const score_step_t part_b[] = {
    {E5, H}, {C5, H},
    {D5, H}, {B4, H},
    {C5, H}, {A4, H},
    {Gs4, H}, {B4, H},
    {E5, H}, {C5, H},
    {D5, H}, {B4, H},
    {C5, Q}, {E5, Q}, {A5, H},
    {Gs5, W},
};

/* Chords, one per bar */
typedef struct { uint8_t bass; uint8_t arp[3]; } chord_t;
static const chord_t Em = {52, {64, 67, 71}};
static const chord_t Am = {57, {64, 69, 72}};
static const chord_t EM = {52, {64, 68, 71}};
static const chord_t Dm = {50, {62, 65, 69}};
static const chord_t CM = {48, {64, 67, 72}};

static const chord_t *const chords_a[8] = {&Em, &Am, &EM, &Am, &Dm, &CM, &EM, &Am};
static const chord_t *const chords_b[8] = {&Am, &EM, &Am, &EM, &Am, &EM, &Am, &EM};

#define BARS        32      /* A A B A */
#define N_PARTS     4

/* Generated tracks (sized for 32 bars) */
static score_step_t melody[4 * (sizeof(part_a) / sizeof(part_a[0]))];
static score_step_t bass[BARS * 8];
static score_step_t arp[BARS * 16];
static score_step_t kick[BARS * 2];
static score_step_t snare[BARS * 4];
static score_step_t hat[BARS * 8];

static player_track_t tracks[6];

static size_t append(score_step_t *dst, size_t n, const score_step_t *src, size_t count)
{
    for (size_t i = 0; i < count; i++) dst[n++] = src[i];
    return n;
}

static void build(void)
{
    static const char form[N_PARTS] = {'A', 'A', 'B', 'A'};
    size_t nm = 0, nb = 0, na = 0, nk = 0, ns = 0, nh = 0;

    for (int part = 0; part < N_PARTS; part++) {
        const bool is_a = form[part] == 'A';
        if (is_a) nm = append(melody, nm, part_a, sizeof(part_a) / sizeof(part_a[0]));
        else      nm = append(melody, nm, part_b, sizeof(part_b) / sizeof(part_b[0]));

        const chord_t *const *chords = is_a ? chords_a : chords_b;
        for (int bar = 0; bar < 8; bar++) {
            const chord_t *c = chords[bar];
            /* Octave-jumping 8th-note bass */
            for (int i = 0; i < 8; i++) {
                bass[nb++] = (score_step_t){(uint8_t)(c->bass + (i & 1) * 12), E};
            }
            /* 16th-note arpeggio: root, 3rd, 5th, 3rd */
            static const int order[4] = {0, 1, 2, 1};
            for (int i = 0; i < 16; i++) {
                arp[na++] = (score_step_t){c->arp[order[i & 3]], 0.25f};
            }
            /* Drums: kick on 1 and 3, snare on 2 and 4, hats on the off-beats */
            kick[nk++] = (score_step_t){KICK_NOTE, H};
            kick[nk++] = (score_step_t){KICK_NOTE, H};
            snare[ns++] = (score_step_t){REST, Q};
            snare[ns++] = (score_step_t){SNARE_NOTE, Q};
            snare[ns++] = (score_step_t){REST, Q};
            snare[ns++] = (score_step_t){SNARE_NOTE, Q};
            for (int i = 0; i < 4; i++) {
                hat[nh++] = (score_step_t){REST, E};
                hat[nh++] = (score_step_t){HAT_NOTE, E};
            }
        }
    }

    tracks[0] = (player_track_t){melody, nm, SYN_LEAD, 0.55f, 0.85f};
    tracks[1] = (player_track_t){arp,    na, SYN_ARP,  0.16f, 0.60f};
    tracks[2] = (player_track_t){bass,   nb, SYN_BASS, 0.60f, 0.80f};
    tracks[3] = (player_track_t){kick,   nk, SYN_KICK, 0.80f, 0.10f};
    tracks[4] = (player_track_t){snare,  ns, SYN_SNARE, 0.35f, 0.10f};
    tracks[5] = (player_track_t){hat,    nh, SYN_HAT,  0.18f, 0.10f};
}

static void setup(void)
{
    /* One osc per voice; osc numbers below are voice-relative.
     * bp0 (A) = amp envelope as time_ms,value pairs; the last pair is the release. */
    amy_add_message("i1iv2in1Z");
    amy_add_message("i1v0w1d0.25A3,1,180,0.55,90,0Z");              /* lead: 25% pulse */
    amy_add_message("i2iv2in1Z");
    amy_add_message("i2v0w1d0.5A1,1,70,0.4,25,0Z");                 /* arp: 50% pulse */
    amy_add_message("i3iv2in1Z");
    amy_add_message("i3v0w4A2,1,120,0.7,40,0Z");                    /* bass: triangle */
    amy_add_message("i4iv1in1Z");
    /* kick: sine, freq = note + 2 octaves * EG1, EG1 falls 1 -> 0 in 45 ms */
    amy_add_message("i4v0w0f0,1,0,0,2,0,1B0,1,45,0,0,0A1,1,160,0,10,0Z");
    amy_add_message("i5iv1in1Z");
    amy_add_message("i5v0w5G2F1800R1.2A1,1,110,0,20,0Z");           /* snare: band-passed noise */
    amy_add_message("i6iv1in1Z");
    amy_add_message("i6v0w5G3F7000A1,1,28,0,10,0Z");                /* hat: high-passed noise */

    /* A little echo for that demoscene shimmer: level, delay ms, max delay ms, feedback */
    amy_add_message("M0.18,312,500,0.3Z");
}

const amy_song_t *song_korobeiniki(void)
{
    static amy_song_t song = {
        .name = "Korobeiniki (Tetris theme A) - chiptune",
        .bpm = 144.0f,
        .max_oscs = 16,         /* 9 used (2+2+2+1+1+1), plus spare */
        .volume = 3.0f,         /* pulse/noise voices at these velocities peak ~-17 dBFS at 1.0 */
        .setup = setup,
        .tracks = tracks,
        .track_count = 6,
    };
    build();
    return &song;
}
