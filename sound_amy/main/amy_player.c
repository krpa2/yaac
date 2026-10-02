#include <string.h>
#include "amy.h"
#include "amy_player.h"

static void send_note(uint32_t time_ms, uint8_t synth, uint8_t note, float velocity)
{
    amy_event e = amy_default_event();
    e.time = time_ms;
    e.synth = synth;
    e.midi_note = note;
    e.velocity = velocity;      /* 0 = note off */
    amy_add_event(&e);
}

void player_start(player_t *p, const amy_song_t *song, uint32_t start_ms)
{
    memset(p, 0, sizeof(*p));
    p->song = song;
    p->ms_per_beat = 60000.0 / song->bpm;
    for (size_t t = 0; t < PLAYER_MAX_TRACKS; t++) {
        p->pos[t].next_on_ms = start_ms;
    }
}

bool player_feed(player_t *p, uint32_t horizon_ms)
{
    bool playing = false;
    for (size_t t = 0; t < p->song->track_count && t < PLAYER_MAX_TRACKS; t++) {
        const player_track_t *trk = &p->song->tracks[t];
        player_track_pos_t *pos = &p->pos[t];
        for (;;) {
            const bool off_due = pos->held_note && pos->off_ms <= horizon_ms;
            const bool on_due = pos->index < trk->count && pos->next_on_ms <= horizon_ms;
            if (off_due && (!on_due || pos->off_ms <= pos->next_on_ms)) {
                send_note((uint32_t)pos->off_ms, trk->synth, pos->held_note, 0.0f);
                pos->held_note = 0;
            } else if (on_due) {
                const score_step_t *st = &trk->steps[pos->index++];
                const double len = st->beats * p->ms_per_beat;
                if (st->note != REST) {
                    send_note((uint32_t)pos->next_on_ms, trk->synth, st->note, trk->velocity);
                    pos->held_note = st->note;
                    pos->off_ms = pos->next_on_ms + len * trk->gate;
                }
                pos->next_on_ms += len;
            } else {
                break;
            }
        }
        if (pos->index < trk->count || pos->held_note) {
            playing = true;
        }
    }
    return playing;
}
