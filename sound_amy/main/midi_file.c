#include <stdlib.h>
#include <string.h>
#include "midi_file.h"

typedef struct {
    uint32_t tick;
    uint32_t seq;       /* file order, keeps the sort stable */
    uint8_t  status, d1, d2;
} raw_event_t;

typedef struct {
    uint32_t tick;
    uint32_t us_per_quarter;
} tempo_t;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static bool read_vlq(const uint8_t *p, size_t len, size_t *i, uint32_t *out)
{
    uint32_t v = 0;
    for (int n = 0; n < 4; n++) {
        if (*i >= len) return false;
        const uint8_t c = p[(*i)++];
        v = (v << 7) | (c & 0x7f);
        if (!(c & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

/* Note-offs sort before note-ons at the same tick so a repeated note isn't cut short */
static int cmp_raw(const void *a, const void *b)
{
    const raw_event_t *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    const int xon = (x->status & 0xF0) == 0x90, yon = (y->status & 0xF0) == 0x90;
    if (xon != yon) return xon - yon;
    return x->seq < y->seq ? -1 : (x->seq > y->seq);
}

static int cmp_tempo(const void *a, const void *b)
{
    const tempo_t *x = a, *y = b;
    return x->tick < y->tick ? -1 : (x->tick > y->tick);
}

int midi_file_load(const uint8_t *data, size_t len, midi_song_t *song)
{
    memset(song, 0, sizeof(*song));
    for (int c = 0; c < 16; c++) song->first_program[c] = -1;
    if (len < 14 || memcmp(data, "MThd", 4) != 0) return -1;

    const uint32_t hdr_len = be32(data + 4);
    const uint16_t ntracks = be16(data + 10);
    const uint16_t division = be16(data + 12);
    if (division & 0x8000) return -2;           /* SMPTE timing not supported */

    size_t cap = 1024, n = 0, tcap = 16, nt = 0;
    raw_event_t *raw = malloc(cap * sizeof(*raw));
    tempo_t *tempos = malloc(tcap * sizeof(*tempos));
    if (!raw || !tempos) goto fail;

    size_t pos = 8 + hdr_len;
    uint32_t seq = 0;
    for (uint16_t t = 0; t < ntracks; t++) {
        if (pos + 8 > len || memcmp(data + pos, "MTrk", 4) != 0) break;
        const uint32_t tlen = be32(data + pos + 4);
        const uint8_t *trk = data + pos + 8;
        const size_t end = (pos + 8 + tlen <= len) ? tlen : len - pos - 8;
        pos += 8 + tlen;

        size_t i = 0;
        uint32_t tick = 0;
        uint8_t running = 0;
        while (i < end) {
            uint32_t delta;
            if (!read_vlq(trk, end, &i, &delta) || i >= end) break;
            tick += delta;
            uint8_t st = trk[i];
            if (st == 0xFF) {                   /* meta event */
                if (i + 2 > end) break;
                const uint8_t type = trk[i + 1];
                uint32_t mlen;
                i += 2;
                if (!read_vlq(trk, end, &i, &mlen) || i + mlen > end) break;
                if (type == 0x51 && mlen == 3) {
                    if (nt == tcap) {
                        tcap *= 2;
                        tempo_t *nt_ = realloc(tempos, tcap * sizeof(*tempos));
                        if (!nt_) goto fail;
                        tempos = nt_;
                    }
                    tempos[nt++] = (tempo_t){tick, (uint32_t)trk[i] << 16 | trk[i + 1] << 8 | trk[i + 2]};
                }
                i += mlen;
                if (type == 0x2F) break;        /* end of track */
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {     /* sysex: skip */
                uint32_t slen;
                i++;
                if (!read_vlq(trk, end, &i, &slen) || i + slen > end) break;
                i += slen;
                continue;
            }
            if (st & 0x80) {
                running = st;
                i++;
            } else if (!running) {
                break;                          /* data byte without status: corrupt */
            }
            const uint8_t type = running & 0xF0;
            const int nbytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (i + nbytes > end) break;
            raw_event_t ev = {tick, seq++, running, trk[i], nbytes == 2 ? trk[i + 1] : 0};
            i += nbytes;
            if (type == 0x90 && ev.d2 == 0) ev.status = 0x80 | (running & 0x0F);
            if (type == 0xA0 || type == 0xD0) continue;    /* aftertouch: ignored */
            if (n == cap) {
                cap *= 2;
                raw_event_t *nr = realloc(raw, cap * sizeof(*raw));
                if (!nr) goto fail;
                raw = nr;
            }
            raw[n++] = ev;
        }
    }

    qsort(raw, n, sizeof(*raw), cmp_raw);
    qsort(tempos, nt, sizeof(*tempos), cmp_tempo);

    song->events = malloc((n ? n : 1) * sizeof(midi_event_t));
    if (!song->events) goto fail;

    /* Ticks -> ms through the tempo map; collect per-channel stats */
    uint8_t held[16][128];
    uint8_t held_count[16] = {0};
    memset(held, 0, sizeof(held));
    size_t ti = 0;
    uint32_t seg_tick = 0, us_per_q = 500000;  /* 120 BPM until the first tempo event */
    double seg_ms = 0.0;
    for (size_t k = 0; k < n; k++) {
        const raw_event_t *r = &raw[k];
        while (ti < nt && tempos[ti].tick <= r->tick) {
            seg_ms += (double)(tempos[ti].tick - seg_tick) * us_per_q / 1000.0 / division;
            seg_tick = tempos[ti].tick;
            us_per_q = tempos[ti].us_per_quarter;
            ti++;
        }
        const double ms = seg_ms + (double)(r->tick - seg_tick) * us_per_q / 1000.0 / division;
        song->events[k] = (midi_event_t){(uint32_t)(ms + 0.5), r->status, r->d1, r->d2};

        const uint8_t ch = r->status & 0x0F, type = r->status & 0xF0, note = r->d1 & 0x7F;
        if (type == 0x90) {
            song->notes[ch]++;
            if (held[ch][note] < 255) held[ch][note]++;
            held_count[ch]++;
            if (held_count[ch] > song->max_poly[ch]) song->max_poly[ch] = held_count[ch];
        } else if (type == 0x80 && held[ch][note]) {
            held[ch][note]--;
            held_count[ch]--;
        } else if (type == 0xC0 && song->first_program[ch] < 0) {
            song->first_program[ch] = r->d1;
        }
        song->length_ms = song->events[k].ms;
    }
    song->count = n;
    free(raw);
    free(tempos);
    return 0;

fail:
    free(raw);
    free(tempos);
    midi_file_free(song);
    return -3;
}

void midi_file_free(midi_song_t *song)
{
    free(song->events);
    song->events = NULL;
    song->count = 0;
}
