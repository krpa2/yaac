/*
 * PC simulation of the "sound" firmware.
 *
 * Compiles the very same piano_synth.c, sequencer.c and imperial_march.c as
 * the ESP32 build and writes the result to WAV files:
 *
 *   imperial_march_dac.wav            the PCM the firmware sends to the ES8311 (same code; float
 *                                    libm differences may change the last bit or so)
 *   imperial_march_small_speaker.wav  same signal through a crude micro-speaker model
 *
 * Usage: pc_sim [output_prefix] [speaker_resonance_hz] [speaker_q]
 *   defaults: imperial_march 600 1.2
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piano_synth.h"
#include "sequencer.h"
#include "imperial_march.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BLOCK_FRAMES    256
#define TAIL_SECONDS    1.5f

/* ---- WAV writer (16-bit PCM, stereo) ---------------------------------- */

static void put_u16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void put_u32(FILE *f, uint32_t v) { put_u16(f, v & 0xffff); put_u16(f, v >> 16); }

static int write_wav(const char *path, const int16_t *pcm, size_t frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    const uint32_t data_bytes = (uint32_t)(frames * 2 * sizeof(int16_t));
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16);
    put_u16(f, 1);                              /* PCM */
    put_u16(f, 2);                              /* channels */
    put_u32(f, SYNTH_SAMPLE_RATE);
    put_u32(f, SYNTH_SAMPLE_RATE * 2 * 2);      /* byte rate */
    put_u16(f, 4);                              /* block align */
    put_u16(f, 16);                             /* bits per sample */
    fwrite("data", 1, 4, f); put_u32(f, data_bytes);
    for (size_t i = 0; i < frames * 2; i++) {
        put_u16(f, (uint16_t)pcm[i]);
    }
    fclose(f);
    return 0;
}

/* ---- Micro-speaker model ---------------------------------------------- */
/* A small speaker is roughly a 2nd-order high-pass at its resonance (Fs):
 * almost nothing comes out below it, with a bump around it. Cone breakup
 * above ~8 kHz is modelled as a gentle low-pass. */

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

static void biquad_highpass(biquad_t *q, float fc, float Q)
{
    const float w = 2.0f * (float)M_PI * fc / SYNTH_SAMPLE_RATE;
    const float alpha = sinf(w) / (2.0f * Q), c = cosf(w), a0 = 1.0f + alpha;
    q->b0 = (1.0f + c) / 2.0f / a0; q->b1 = -(1.0f + c) / a0; q->b2 = q->b0;
    q->a1 = -2.0f * c / a0; q->a2 = (1.0f - alpha) / a0; q->z1 = q->z2 = 0;
}

static void biquad_lowpass(biquad_t *q, float fc, float Q)
{
    const float w = 2.0f * (float)M_PI * fc / SYNTH_SAMPLE_RATE;
    const float alpha = sinf(w) / (2.0f * Q), c = cosf(w), a0 = 1.0f + alpha;
    q->b0 = (1.0f - c) / 2.0f / a0; q->b1 = (1.0f - c) / a0; q->b2 = q->b0;
    q->a1 = -2.0f * c / a0; q->a2 = (1.0f - alpha) / a0; q->z1 = q->z2 = 0;
}

static float biquad_run(biquad_t *q, float x)
{
    const float y = q->b0 * x + q->z1;      /* transposed direct form II */
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static void apply_speaker(const int16_t *in, int16_t *out, size_t frames, float fs_hz, float q)
{
    biquad_t hp, lp;
    biquad_highpass(&hp, fs_hz, q);
    biquad_lowpass(&lp, 8000.0f, 0.707f);
    size_t clipped = 0;
    for (size_t i = 0; i < frames; i++) {
        float y = biquad_run(&lp, biquad_run(&hp, in[2 * i] / 32768.0f));
        if (y > 1.0f || y < -1.0f) {
            clipped++;
            y = y > 0 ? 1.0f : -1.0f;
        }
        out[2 * i] = out[2 * i + 1] = (int16_t)(y * 32767.0f);
    }
    if (clipped) {
        printf("  speaker model clipped %zu samples\n", clipped);
    }
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *prefix = argc > 1 ? argv[1] : "imperial_march";
    const float spk_fs = argc > 2 ? (float)atof(argv[2]) : 600.0f;
    const float spk_q = argc > 3 ? (float)atof(argv[3]) : 1.2f;

    /* Render exactly the way the firmware does, block by block */
    size_t cap = SYNTH_SAMPLE_RATE * 60, frames = 0;
    int16_t *pcm = malloc(cap * 2 * sizeof(int16_t));
    int16_t block[BLOCK_FRAMES * 2];
    sequencer_t seq;

    piano_synth_init();
    sequencer_start(&seq, imperial_march_tracks, imperial_march_track_count, imperial_march_bpm);
    size_t tail = (size_t)(TAIL_SECONDS * SYNTH_SAMPLE_RATE / BLOCK_FRAMES);
    bool playing = true;
    while (playing || tail--) {
        if (playing) {
            playing = sequencer_render(&seq, block, BLOCK_FRAMES);
        } else {
            piano_render(block, BLOCK_FRAMES);  /* let release tails ring out */
        }
        if (frames + BLOCK_FRAMES > cap) {
            cap *= 2;
            pcm = realloc(pcm, cap * 2 * sizeof(int16_t));
        }
        memcpy(&pcm[frames * 2], block, sizeof(block));
        frames += BLOCK_FRAMES;
    }

    int16_t peak = 0;
    for (size_t i = 0; i < frames * 2; i++) {
        int16_t a = pcm[i] < 0 ? (int16_t)-(pcm[i] + 1) : pcm[i];
        if (a > peak) peak = a;
    }
    printf("Rendered %.2f s, peak %.1f dBFS\n", (double)frames / SYNTH_SAMPLE_RATE,
           20.0 * log10((peak + 1) / 32768.0));

    char path[512];
    snprintf(path, sizeof(path), "%s_dac.wav", prefix);
    if (write_wav(path, pcm, frames)) return 1;
    printf("Wrote %s\n", path);

    int16_t *spk = malloc(frames * 2 * sizeof(int16_t));
    apply_speaker(pcm, spk, frames, spk_fs, spk_q);
    snprintf(path, sizeof(path), "%s_small_speaker.wav", prefix);
    if (write_wav(path, spk, frames)) return 1;
    printf("Wrote %s (speaker model: resonance %.0f Hz, Q %.2f, 8 kHz roll-off)\n", path, spk_fs, spk_q);

    free(spk);
    free(pcm);
    return 0;
}
