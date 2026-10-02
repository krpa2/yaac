#include <stdio.h>
#include <string.h>
#include "amy.h"
#include "midi_player.h"

#define DRUM_CHANNEL    9       /* MIDI channel 10 */
#define DRUM_PATCH      258     /* AMY General MIDI drum kit (built-in PCM samples) */

extern int instrument_get_oscs_per_voice(int instrument_number);
extern int instrument_get_num_voices(int instrument_number, uint16_t *amy_voices);

/* General MIDI program -> AMY patch (0-127 Juno-6, 128-255 DX7). Chosen by
 * name/character; AMY has no GM bank, so this is an approximation. */
static const uint16_t gm_map[128] = {
    /* Piano */          135, 136, 137, 160, 138, 162, 146, 147,
    /* Chrom. perc. */   166, 213, 219, 148, 149, 215, 153, 150,
    /* Organ */          144, 172,   9, 145, 107, 180, 209,  77,
    /* Guitar */          73, 139, 140, 182,  27,  32,  33,  72,
    /* Bass */           142, 143, 190, 191,  87, 111,  36,  31,
    /* Strings */         65,  65, 122, 120,   4,  22, 188, 155,
    /* Ensemble */        64, 131,  21,  23,   6, 157, 236, 134,
    /* Brass */            2, 128, 129, 121, 108,  26, 232,   0,
    /* Reed */           196, 196, 196, 196, 194,  25, 197, 195,
    /* Pipe */           192, 151, 208,   3,  69, 193, 156, 112,
    /* Synth lead */      34,  32, 179,  33, 141, 236, 165, 224,
    /* Synth pad */      101,  47,  94,   6,  74,  95, 101,  48,
    /* Synth FX */        72,  63, 116,  90, 102, 125,  50,  84,
    /* Ethnic */         181, 187,  28, 150,  52,  86,  65, 194,
    /* Percussive */     118, 220, 154, 221,  45,  44, 245,  78,
    /* Sound FX */        46,  71,  92, 156, 249,  81,  99, 250,
};

uint16_t midi_gm_to_amy_patch(uint8_t program)
{
    return gm_map[program & 0x7F];
}

/* GM programs played on raw AMY oscillators instead of a Juno/DX7 patch: the
 * GM "synth lead" waves map directly to chip-style voices. One osc per voice;
 * A = amp envelope (time_ms,value pairs, last pair is the release). */
static const struct { uint8_t program; const char *name; const char *osc; } chip_voices[] = {
    {80, "chip square (25% pulse)", "w1d0.25A3,1,180,0.6,90,0"},
    {81, "chip saw",                "w3A3,1,180,0.6,90,0"},
};
#define CHIP_PATCH_FLAG 0x8000      /* p->patch[] marker: chip voice, low bits = GM program */

const char *midi_gm_chip_voice(uint8_t program)
{
    for (size_t i = 0; i < sizeof(chip_voices) / sizeof(chip_voices[0]); i++) {
        if (chip_voices[i].program == program) return chip_voices[i].name;
    }
    return NULL;
}

static bool send_chip_voice(uint8_t ch, uint8_t program, uint16_t voices)
{
    for (size_t i = 0; i < sizeof(chip_voices) / sizeof(chip_voices[0]); i++) {
        if (chip_voices[i].program != program) continue;
        char msg[96];
        snprintf(msg, sizeof(msg), "i%div%uin1Z", ch + 1, voices);
        amy_add_message(msg);
        snprintf(msg, sizeof(msg), "i%dv0%sZ", ch + 1, chip_voices[i].osc);
        amy_add_message(msg);
        return true;
    }
    return false;
}

static void send_patch(uint8_t ch, uint16_t patch, uint16_t voices)
{
    amy_event e = amy_default_event();
    e.synth = ch + 1;
    e.patch_number = patch;
    if (voices) e.num_voices = voices;
    amy_add_event(&e);
}

void midi_player_setup(midi_player_t *p, const midi_song_t *song)
{
    memset(p, 0, sizeof(*p));
    p->song = song;
    for (int ch = 0; ch < 16; ch++) {
        p->volume[ch] = 1.0f;
        p->expression[ch] = 1.0f;
        if (!song->notes[ch]) continue;
        if (ch == DRUM_CHANNEL) {
            p->patch[ch] = DRUM_PATCH;
            send_patch(ch, DRUM_PATCH, 0);  /* the kit patch defines its own voice */
            continue;
        }
        uint16_t voices = song->max_poly[ch];
        if (voices > MIDI_MAX_VOICES_PER_CHANNEL) voices = MIDI_MAX_VOICES_PER_CHANNEL;
        if (voices < 1) voices = 1;
        const uint8_t program = song->first_program[ch] < 0 ? 0 : (uint8_t)song->first_program[ch];
        if (send_chip_voice(ch, program, voices)) {
            p->patch[ch] = CHIP_PATCH_FLAG | program;
            continue;
        }
        p->patch[ch] = midi_gm_to_amy_patch(program);
        send_patch(ch, p->patch[ch], voices);
    }
}

int midi_player_count_oscs(const midi_song_t *song)
{
    int total = 0;
    for (int ch = 0; ch < 16; ch++) {
        if (!song->notes[ch]) continue;
        const int voices = instrument_get_num_voices(ch + 1, NULL);
        const int per_voice = instrument_get_oscs_per_voice(ch + 1);
        if (voices > 0 && per_voice > 0) total += voices * per_voice;
    }
    return total;
}

int midi_player_amy_start(midi_player_t *p, const midi_song_t *song, const amy_config_t *config, float volume)
{
    amy_config_t cfg = *config;

    /* Pass 1: generous pool, define the synths, count what they occupy */
    cfg.max_oscs = 512;
    amy_start(cfg);
    midi_player_setup(p, song);
    amy_execute_deltas();
    const int used = midi_player_count_oscs(song);
    amy_stop();

    /* Pass 2: pool sized to the song (+ a little headroom) */
    cfg.max_oscs = (uint16_t)(used + 8);
    amy_start(cfg);
    amy_event e = amy_default_event();
    e.volume = volume;
    amy_add_event(&e);
    midi_player_setup(p, song);
    return cfg.max_oscs;
}

void midi_player_start(midi_player_t *p, uint32_t start_ms)
{
    p->index = 0;
    p->start_ms = start_ms;
}

static void send_level(midi_player_t *p, uint8_t ch, uint32_t t)
{
    /* GM volume/expression curves are roughly 40*log10(v/127) dB, i.e. squared amplitude */
    const float v = p->volume[ch] * p->expression[ch];
    amy_event e = amy_default_event();
    e.time = t;
    e.synth = ch + 1;
    e.synth_level = v * v;
    amy_add_event(&e);
}

bool midi_player_feed(midi_player_t *p, uint32_t horizon_ms)
{
    const midi_song_t *s = p->song;
    while (p->index < s->count) {
        const midi_event_t *ev = &s->events[p->index];
        const uint32_t t = p->start_ms + ev->ms;
        if (t > horizon_ms) break;
        p->index++;

        const uint8_t ch = ev->status & 0x0F;
        if (!s->notes[ch]) continue;    /* no synth for a channel without notes */
        amy_event e = amy_default_event();
        e.time = t;
        e.synth = ch + 1;
        switch (ev->status & 0xF0) {
        case 0x90:
            e.midi_note = ev->d1;
            e.velocity = ev->d2 / 127.0f;
            amy_add_event(&e);
            break;
        case 0x80:
            if (ch == DRUM_CHANNEL) break;      /* one-shot drum samples */
            e.midi_note = ev->d1;
            e.velocity = 0.0f;
            amy_add_event(&e);
            break;
        case 0xB0:
            if (ev->d1 == 7) {
                p->volume[ch] = ev->d2 / 127.0f;
                send_level(p, ch, t);
            } else if (ev->d1 == 11) {
                p->expression[ch] = ev->d2 / 127.0f;
                send_level(p, ch, t);
            } else if (ev->d1 == 64) {
                e.pedal = ev->d2 >= 64;
                amy_add_event(&e);
            }
            break;
        case 0xC0:
            if (ch != DRUM_CHANNEL && !(p->patch[ch] & CHIP_PATCH_FLAG) && !midi_gm_chip_voice(ev->d1)) {
                const uint16_t patch = midi_gm_to_amy_patch(ev->d1);
                /* A patch change rebuilds the synth's voices. Only allow it within the
                 * same family (Juno/DX7): the osc pool was sized for the first patch. */
                if (patch != p->patch[ch] && (patch >= 128) == (p->patch[ch] >= 128)) {
                    p->patch[ch] = patch;
                    e.patch_number = patch;
                    amy_add_event(&e);
                }
            }
            break;
        default:
            break;                              /* pitch bend etc. not handled yet */
        }
    }
    return p->index < s->count;
}
