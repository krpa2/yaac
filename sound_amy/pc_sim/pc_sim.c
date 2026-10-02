/*
 * PC render of the sound_amy firmware: same AMY sources, same amy_player.c and
 * song_*.c, output to WAV instead of the ES8311.
 *
 *   pc_sim                  korobeiniki_chiptune.wav, imperial_march_piano.wav (song_*.c)
 *   pc_sim file.mid ...     <name>.wav for each MIDI file (midi_player.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "amy.h"
#include "amy_player.h"
#include "midi_file.h"
#include "midi_player.h"

#define LOOKAHEAD_MS    50

/* AMY's examples.c (not used here) expects the host to provide this */
void delay_ms(uint32_t ms) { (void)ms; }
#define TAIL_SECONDS    1.5

static void put_u16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void put_u32(FILE *f, uint32_t v) { put_u16(f, v & 0xffff); put_u16(f, v >> 16); }

static int write_wav(const char *path, const int16_t *pcm, size_t frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    const uint32_t data_bytes = (uint32_t)(frames * AMY_NCHANS * sizeof(int16_t));
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, AMY_NCHANS);
    put_u32(f, AMY_SAMPLE_RATE);
    put_u32(f, AMY_SAMPLE_RATE * AMY_NCHANS * 2);
    put_u16(f, AMY_NCHANS * 2);
    put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, data_bytes);
    for (size_t i = 0; i < frames * AMY_NCHANS; i++) {
        put_u16(f, (uint16_t)pcm[i]);
    }
    fclose(f);
    return 0;
}

static amy_config_t base_config(void)
{
    /* Same AMY configuration as the firmware (minus the ESP multicore helper) */
    amy_config_t cfg = amy_default_config();
    cfg.audio = AMY_AUDIO_IS_NONE;
    cfg.midi = AMY_MIDI_IS_NONE;
    cfg.features.default_synths = 0;
    cfg.features.startup_bleep = 0;
    cfg.platform.multicore = 0;
    cfg.platform.multithread = 0;
    return cfg;
}

static void report(const char *name, const int16_t *pcm, size_t frames, const char *path)
{
    int peak = 0, clipped = 0;
    for (size_t i = 0; i < frames * AMY_NCHANS; i++) {
        int a = abs(pcm[i]);
        if (a > peak) peak = a;
        if (a >= 32767) clipped++;
    }
    printf("%s: %.1f s, peak %.1f dBFS%s -> %s\n", name, (double)frames / AMY_SAMPLE_RATE,
           20.0 * log10((peak + 1) / 32768.0), clipped ? " (hits full scale)" : "", path);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*len);
    if (buf && fread(buf, 1, *len, f) != *len) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

static int render_midi(const char *mid_path, float volume)
{
    size_t len = 0;
    uint8_t *data = read_file(mid_path, &len);
    if (!data) { perror(mid_path); return -1; }
    midi_song_t song;
    if (midi_file_load(data, len, &song) != 0) {
        fprintf(stderr, "%s: not a MIDI file I can read\n", mid_path);
        free(data);
        return -1;
    }
    printf("%s: %zu events, %.1f s\n", mid_path, song.count, song.length_ms / 1000.0);
    for (int ch = 0; ch < 16; ch++) {
        if (!song.notes[ch]) continue;
        const uint8_t prog = song.first_program[ch] < 0 ? 0 : (uint8_t)song.first_program[ch];
        const char *chip = midi_gm_chip_voice(prog);
        if (ch == 9) printf("  ch%2d  drums           %5u notes  max poly %u -> AMY patch 258\n", ch + 1, song.notes[ch], song.max_poly[ch]);
        else if (chip) printf("  ch%2d  GM program %3d  %5u notes  max poly %u -> %s\n", ch + 1,
                              song.first_program[ch], song.notes[ch], song.max_poly[ch], chip);
        else printf("  ch%2d  GM program %3d  %5u notes  max poly %u -> AMY patch %u\n", ch + 1,
                    song.first_program[ch], song.notes[ch], song.max_poly[ch], midi_gm_to_amy_patch(prog));
    }

    amy_config_t cfg = base_config();
    midi_player_t player;
    const int oscs = midi_player_amy_start(&player, &song, &cfg, volume);
    printf("  AMY osc pool: %d\n", oscs);

    size_t cap = (size_t)AMY_SAMPLE_RATE * 60, frames = 0;
    int16_t *pcm = malloc(cap * AMY_NCHANS * sizeof(int16_t));
    midi_player_start(&player, amy_sysclock() + 100);
    bool playing = true;
    size_t tail_blocks = (size_t)(TAIL_SECONDS * AMY_SAMPLE_RATE / AMY_BLOCK_SIZE);
    while (playing || tail_blocks--) {
        if (playing) playing = midi_player_feed(&player, amy_sysclock() + LOOKAHEAD_MS);
        int16_t *block = amy_simple_fill_buffer();
        if (frames + AMY_BLOCK_SIZE > cap) {
            cap *= 2;
            pcm = realloc(pcm, cap * AMY_NCHANS * sizeof(int16_t));
        }
        memcpy(&pcm[frames * AMY_NCHANS], block, AMY_BLOCK_SIZE * AMY_NCHANS * sizeof(int16_t));
        frames += AMY_BLOCK_SIZE;
    }
    amy_stop();

    /* <name>.wav in the current directory */
    char out[512];
    const char *base = mid_path;
    for (const char *c = mid_path; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    snprintf(out, sizeof(out), "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    strncat(out, ".wav", sizeof(out) - strlen(out) - 1);
    report(base, pcm, frames, out);
    int rc = write_wav(out, pcm, frames);
    free(pcm);
    midi_file_free(&song);
    free(data);
    return rc;
}

static int render_song(const amy_song_t *song, const char *path)
{
    amy_config_t cfg = base_config();
    cfg.max_oscs = song->max_oscs;
    amy_start(cfg);

    amy_event e = amy_default_event();
    e.volume = song->volume;
    amy_add_event(&e);
    song->setup();

    size_t cap = (size_t)AMY_SAMPLE_RATE * 120, frames = 0;
    int16_t *pcm = malloc(cap * AMY_NCHANS * sizeof(int16_t));
    player_t player;
    player_start(&player, song, amy_sysclock() + 100);

    bool playing = true;
    size_t tail_blocks = (size_t)(TAIL_SECONDS * AMY_SAMPLE_RATE / AMY_BLOCK_SIZE);
    while (playing || tail_blocks--) {
        if (playing) {
            playing = player_feed(&player, amy_sysclock() + LOOKAHEAD_MS);
        }
        /* amy_update() on desktop waits for a miniaudio thread; render directly */
        int16_t *block = amy_simple_fill_buffer();
        if (frames + AMY_BLOCK_SIZE > cap) {
            cap *= 2;
            pcm = realloc(pcm, cap * AMY_NCHANS * sizeof(int16_t));
        }
        memcpy(&pcm[frames * AMY_NCHANS], block, AMY_BLOCK_SIZE * AMY_NCHANS * sizeof(int16_t));
        frames += AMY_BLOCK_SIZE;
    }
    amy_stop();

    report(song->name, pcm, frames, path);
    int rc = write_wav(path, pcm, frames);
    free(pcm);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (render_midi(argv[i], MIDI_VOLUME)) return 1;
        }
        return 0;
    }
    if (render_song(song_korobeiniki(), "korobeiniki_chiptune.wav")) return 1;
    if (render_song(song_imperial_march_piano(), "imperial_march_piano.wav")) return 1;
    return 0;
}
