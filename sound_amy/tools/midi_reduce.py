#!/usr/bin/env python3
"""Reduce a Standard MIDI File to a few channels.

    python midi_reduce.py in.mid out.mid --keep 2,4,6,10 [--move 9:10] [--program 4:29]
                          [--drum-map 20:36,21:38]

--keep     MIDI channels (1-16) to keep, after --move is applied
--move     SRC:DST  re-route a channel (e.g. drum notes mistakenly on 9 -> 10)
--program  CH:PROG  force the General MIDI program (0-127) of a channel
--drum-map SRC:DST  re-map note numbers on drum channel 10 (e.g. non-GM kit notes)

Meta events (tempo, time signature, track names, ...) are kept; channel events
on other channels are dropped; tracks left with no channel events are removed
(except the first, which carries the tempo map). Drum channel 10 gets no
program changes.
"""
import argparse
import struct


def read_vlq(b, i):
    v = 0
    while True:
        c = b[i]
        i += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, i


def write_vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))


def parse_tracks(data):
    if data[:4] != b"MThd":
        raise ValueError("not a MIDI file")
    hdr_len, fmt, ntrk, division = struct.unpack(">IHHH", data[4:14])
    pos = 8 + hdr_len
    tracks = []
    for _ in range(ntrk):
        if data[pos:pos + 4] != b"MTrk":
            break
        ln = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        tr = data[pos + 8:pos + 8 + ln]
        pos += 8 + ln
        events, i, tick, running = [], 0, 0, 0
        while i < len(tr):
            dt, i = read_vlq(tr, i)
            tick += dt
            s = tr[i]
            if s == 0xFF:
                typ = tr[i + 1]
                ln2, j = read_vlq(tr, i + 2)
                events.append((tick, "meta", bytes([0xFF, typ]) + write_vlq(ln2) + tr[j:j + ln2], typ))
                i = j + ln2
            elif s in (0xF0, 0xF7):
                ln2, j = read_vlq(tr, i + 1)
                events.append((tick, "sysex", tr[i:j + ln2], None))
                i = j + ln2
            else:
                if s & 0x80:
                    running = s
                    i += 1
                n = 1 if running & 0xF0 in (0xC0, 0xD0) else 2
                events.append((tick, "chan", bytes([running]) + tr[i:i + n], None))
                i += n
        tracks.append(events)
    return division, tracks


def reduce(data, keep, move, programs, drum_map):
    division, tracks = parse_tracks(data)
    out_tracks = []
    for t, events in enumerate(tracks):
        kept, has_chan = [], False
        for tick, kind, raw, typ in events:
            if kind == "meta":
                if typ == 0x2F:
                    continue                    # end-of-track re-added below
                kept.append((tick, raw))
            elif kind == "chan":
                ch = (raw[0] & 0x0F) + 1
                ch = move.get(ch, ch)
                if ch not in keep:
                    continue
                status = raw[0] & 0xF0
                if status == 0xC0:
                    if ch == 10:
                        continue                # drum channel: no program changes
                    if ch in programs:
                        raw = bytes([0xC0, programs[ch]])
                if ch == 10 and status in (0x80, 0x90) and raw[1] in drum_map:
                    raw = bytes([raw[0], drum_map[raw[1]]]) + raw[2:]
                raw = bytes([status | (ch - 1)]) + raw[1:]
                kept.append((tick, raw))
                has_chan = True
        if has_chan or t == 0:
            end = max([tick for tick, _ in kept], default=0)
            out_tracks.append((kept, end))

    # Forced programs for channels that never had a program change
    for ch, prog in programs.items():
        if ch == 10 or ch not in keep:
            continue
        has_pc = any((raw[0] & 0xF0) == 0xC0 and (raw[0] & 0x0F) == ch - 1
                     for trk, _ in out_tracks for _, raw in trk)
        if not has_pc:
            out_tracks[0][0].insert(0, (0, bytes([0xC0 | (ch - 1), prog])))

    out = bytearray(b"MThd" + struct.pack(">IHHH", 6, 1, len(out_tracks), division))
    for kept, end in out_tracks:
        body, last = bytearray(), 0
        for tick, raw in kept:
            body += write_vlq(tick - last) + raw
            last = tick
        body += write_vlq(max(0, end - last)) + b"\xFF\x2F\x00"
        out += b"MTrk" + struct.pack(">I", len(body)) + body
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--keep", required=True, help="channels to keep, e.g. 2,4,6,10")
    ap.add_argument("--move", default="", help="SRC:DST[,SRC:DST]")
    ap.add_argument("--program", default="", help="CH:PROG[,CH:PROG]")
    ap.add_argument("--drum-map", default="", help="SRC:DST[,SRC:DST] note numbers on channel 10")
    a = ap.parse_args()
    keep = {int(c) for c in a.keep.split(",")}
    move = dict(tuple(int(x) for x in m.split(":")) for m in a.move.split(",") if m)
    programs = dict(tuple(int(x) for x in m.split(":")) for m in a.program.split(",") if m)
    drum_map = dict(tuple(int(x) for x in m.split(":")) for m in a.drum_map.split(",") if m)
    with open(a.input, "rb") as f:
        data = f.read()
    reduced = reduce(data, keep, move, programs, drum_map)
    with open(a.output, "wb") as f:
        f.write(reduced)
    print(f"{a.input} ({len(data)} B) -> {a.output} ({len(reduced)} B), channels {sorted(keep)}")


if __name__ == "__main__":
    main()
