#include <math.h>
#include <string.h>
#include "piano_synth.h"

#define SINE_BITS       11
#define SINE_SIZE       (1 << SINE_BITS)
#define PARTIALS        7           /* 6 harmonics + 1 detuned fundamental */
#define ATTACK_SAMPLES  (SYNTH_SAMPLE_RATE * 3 / 1000)
#define RELEASE_SEC     0.12f
#define SILENCE_LEVEL   0.0003f
#define MASTER_GAIN     0.12f       /* worst-case peak (2 voices at attack) ~ -6 dBFS */

typedef struct {
    uint8_t  active;
    uint8_t  releasing;
    uint8_t  note;
    uint32_t age;               /* samples since note-on */
    float    velocity;
    float    env;               /* release envelope, 1.0 until note-off */
    float    rel_mul;
    uint32_t phase[PARTIALS];
    uint32_t inc[PARTIALS];
    float    amp[PARTIALS];
    float    decay[PARTIALS];   /* per-sample multiplier */
} voice_t;

static float   sine_table[SINE_SIZE];
static voice_t voices[SYNTH_MAX_VOICES];

/* Relative level of each partial; last entry is the detuned "second string".
 * Kept mellow because the small onboard speaker exaggerates upper harmonics. */
static const float partial_level[PARTIALS] = {1.0f, 0.35f, 0.15f, 0.07f, 0.03f, 0.015f, 0.40f};
/* String stiffness: higher partials are slightly sharp, like a real piano */
static const float inharmonicity = 0.0004f;

void piano_synth_init(void)
{
    for (int i = 0; i < SINE_SIZE; i++) {
        sine_table[i] = sinf(2.0f * (float)M_PI * i / SINE_SIZE);
    }
    memset(voices, 0, sizeof(voices));
}

static voice_t *alloc_voice(void)
{
    voice_t *quietest = &voices[0];
    for (int i = 0; i < SYNTH_MAX_VOICES; i++) {
        if (!voices[i].active) {
            return &voices[i];
        }
        if (voices[i].env * voices[i].amp[0] < quietest->env * quietest->amp[0]) {
            quietest = &voices[i];
        }
    }
    return quietest;    /* steal the quietest voice */
}

void piano_note_on(uint8_t midi_note, float velocity)
{
    voice_t *v = alloc_voice();
    memset(v, 0, sizeof(*v));

    const float freq = 440.0f * powf(2.0f, (midi_note - 69) / 12.0f);
    /* Higher notes die away faster */
    const float base_rate = 1.1f * powf(2.0f, (midi_note - 60) / 24.0f);

    for (int k = 0; k < PARTIALS; k++) {
        float f;
        float rate;
        if (k == PARTIALS - 1) {
            f = freq * 1.0018f;         /* ~3 cents sharp: gentle chorus/beating */
            rate = base_rate;
        } else {
            const int n = k + 1;
            f = n * freq * sqrtf(1.0f + inharmonicity * n * n);
            rate = base_rate * (1.0f + 0.7f * k);
        }
        if (f >= SYNTH_SAMPLE_RATE / 2) {
            continue;                   /* skip partials above Nyquist */
        }
        v->inc[k] = (uint32_t)(f / SYNTH_SAMPLE_RATE * 4294967296.0f);
        v->amp[k] = partial_level[k];
        v->decay[k] = expf(-rate / SYNTH_SAMPLE_RATE);
    }
    v->note = midi_note;
    v->velocity = velocity;
    v->env = 1.0f;
    v->rel_mul = expf(-1.0f / (RELEASE_SEC / 5.0f * SYNTH_SAMPLE_RATE));
    v->active = 1;
}

void piano_note_off(uint8_t midi_note)
{
    for (int i = 0; i < SYNTH_MAX_VOICES; i++) {
        if (voices[i].active && !voices[i].releasing && voices[i].note == midi_note) {
            voices[i].releasing = 1;
        }
    }
}

void piano_render(int16_t *out, size_t frames)
{
    for (size_t s = 0; s < frames; s++) {
        float mix = 0.0f;
        for (int i = 0; i < SYNTH_MAX_VOICES; i++) {
            voice_t *v = &voices[i];
            if (!v->active) {
                continue;
            }
            float sample = 0.0f;
            for (int k = 0; k < PARTIALS; k++) {
                sample += v->amp[k] * sine_table[v->phase[k] >> (32 - SINE_BITS)];
                v->phase[k] += v->inc[k];
                v->amp[k] *= v->decay[k];
            }
            float attack = v->age < ATTACK_SAMPLES ? (float)v->age / ATTACK_SAMPLES : 1.0f;
            v->age++;
            if (v->releasing) {
                v->env *= v->rel_mul;
            }
            mix += sample * attack * v->env * v->velocity;
            if (v->env * v->amp[0] < SILENCE_LEVEL) {
                v->active = 0;
            }
        }
        /* Linear master gain; the clamp is only a safety net and should never
         * engage (any waveshaping here adds harsh upper harmonics) */
        float x = mix * MASTER_GAIN;
        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        int16_t pcm = (int16_t)(x * 32767.0f);
        out[2 * s] = pcm;
        out[2 * s + 1] = pcm;
    }
}
