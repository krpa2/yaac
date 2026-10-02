#include <string.h>
#include "sequencer.h"
#include "piano_synth.h"

void sequencer_start(sequencer_t *seq, const score_track_t *tracks, size_t track_count, float bpm)
{
    memset(seq, 0, sizeof(*seq));
    seq->tracks = tracks;
    seq->track_count = track_count < SEQ_MAX_TRACKS ? track_count : SEQ_MAX_TRACKS;
    seq->samples_per_beat = SYNTH_SAMPLE_RATE * 60.0f / bpm;
}

bool sequencer_render(sequencer_t *seq, int16_t *out, size_t frames)
{
    bool playing = true;
    size_t filled = 0;
    while (filled < frames) {
        /* Fire every event due at this sample, and find the next one */
        uint64_t next_event = UINT64_MAX;
        playing = false;
        for (size_t t = 0; t < seq->track_count; t++) {
            const score_track_t *trk = &seq->tracks[t];
            seq_track_cursor_t *c = &seq->cur[t];
            if (c->held_note && c->pending_off <= seq->now) {
                piano_note_off(c->held_note);
                c->held_note = 0;
            }
            if (c->index < trk->count && c->next_on <= seq->now) {
                const score_step_t *st = &trk->steps[c->index++];
                const uint64_t len = (uint64_t)(st->beats * seq->samples_per_beat);
                if (st->note != REST) {
                    piano_note_on(st->note, trk->velocity);
                    c->held_note = st->note;
                    c->pending_off = seq->now + (uint64_t)(len * trk->gate);
                }
                c->next_on = seq->now + len;
            }
            if (c->index < trk->count || c->held_note) {
                playing = true;
            }
            if (c->index < trk->count && c->next_on < next_event) {
                next_event = c->next_on;
            }
            if (c->held_note && c->pending_off < next_event) {
                next_event = c->pending_off;
            }
        }
        size_t n = frames - filled;
        if (next_event != UINT64_MAX && next_event - seq->now < n) {
            n = (size_t)(next_event - seq->now);
        }
        piano_render(&out[filled * 2], n);
        filled += n;
        seq->now += n;
    }
    return playing;
}
