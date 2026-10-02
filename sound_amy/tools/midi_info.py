#!/usr/bin/env python3
"""Summarise a Standard MIDI File: tempo, tracks, channels, programs, polyphony,
duplicate tracks and drum notes outside the General MIDI kit range.

    python midi_info.py file.mid
"""
import collections
import struct
import sys


def read_vlq(b, i):
    v = 0
    while True:
        c = b[i]
        i += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, i


def main(path):
    d = open(path, "rb").read()
    hl, fmt, ntrk, div = struct.unpack(">IHHH", d[4:14])
    pos = 8 + hl
    tempos, tracks = [], []
    for t in range(ntrk):
        ln = struct.unpack(">I", d[pos + 4:pos + 8])[0]
        tr = d[pos + 8:pos + 8 + ln]
        pos += 8 + ln
        i = tick = run = 0
        name, notes, progs, chans = "", [], set(), collections.Counter()
        while i < len(tr):
            dt, i = read_vlq(tr, i)
            tick += dt
            s = tr[i]
            if s == 0xFF:
                typ = tr[i + 1]
                l, i = read_vlq(tr, i + 2)
                data = tr[i:i + l]
                i += l
                if typ == 0x03:
                    name = data.decode("latin1")
                elif typ == 0x51:
                    tempos.append((tick, 60e6 / int.from_bytes(data, "big")))
                continue
            if s in (0xF0, 0xF7):
                l, i = read_vlq(tr, i + 1)
                i += l
                continue
            if s & 0x80:
                run = s
                i += 1
            n = 1 if run & 0xF0 in (0xC0, 0xD0) else 2
            data = tr[i:i + n]
            i += n
            ch = (run & 0x0F) + 1
            if run & 0xF0 == 0xC0:
                progs.add((ch, data[0]))
            if run & 0xF0 == 0x90 and data[1] > 0:
                notes.append((tick, data[0], ch))
                chans[ch] += 1
            elif run & 0xF0 in (0x80, 0x90):
                notes.append((tick, -data[0] - 1, ch))   # note off marker
        tracks.append((t, name, chans, notes, progs))

    last = max((n[0] for tr in tracks for n in tr[3]), default=0)
    tm = sorted(tempos) or [(0, 120.0)]
    secs, prev, bpm = 0.0, 0, tm[0][1]
    for tk, b in tm[1:] + [(last, None)]:
        secs += (tk - prev) / div * 60 / bpm
        prev, bpm = tk, (b or bpm)
    bars = last / (div * 4) + 1
    print(f"{path}: format {fmt}, {ntrk} tracks, {div} ticks/quarter, "
          f"tempo {', '.join(f'{b:.0f}' for _, b in tm[:4])}{'...' if len(tm) > 4 else ''} BPM, "
          f"~{secs:.0f} s, ~{bars:.0f} bars (4/4)")

    sigs = {}
    for t, name, chans, notes, progs in tracks:
        ons = [n for n in notes if n[1] >= 0]
        if not ons:
            print(f"  trk{t:2} {name!r:22} (no notes)")
            continue
        p = [n[1] for n in ons]
        nbars = len({int(n[0] / (div * 4)) for n in ons})
        starts = collections.Counter(n[0] for n in ons)
        chordal = sum(1 for n in ons if starts[n[0]] > 1) / len(ons)
        held = collections.Counter()
        cur = poly = 0
        for tick, note, ch in sorted(notes, key=lambda x: (x[0], x[1] >= 0)):
            if note >= 0:
                held[note] += 1
                cur += 1
                poly = max(poly, cur)
            elif held[-note - 1]:
                held[-note - 1] -= 1
                cur -= 1
        sig = tuple((n[0], n[1]) for n in ons[:200])
        dup = [k for k, s in sigs.items() if s == sig]
        sigs[t] = sig
        prog = ",".join(str(pg) for _, pg in sorted(progs)) or "-"
        print(f"  trk{t:2} {name!r:22} ch{','.join(map(str, chans))} prog {prog:>5} notes {len(ons):5} "
              f"pitch {min(p)}-{max(p)} (avg {sum(p) / len(p):4.1f}) poly {poly} chordal {chordal:.2f} "
              f"bars {nbars}/{bars:.0f}{'  DUPLICATE of trk' + ','.join(map(str, dup)) if dup else ''}")

    drum = collections.Counter(n[1] for tr in tracks for n in tr[3] if n[2] == 10 and n[1] >= 0)
    if drum:
        outside = {k: v for k, v in drum.items() if not 35 <= k <= 81}
        print(f"  drum notes (ch10): {sorted(drum.items())}")
        if outside:
            print(f"  !! drum notes outside GM 35-81: {sorted(outside.items())}")


if __name__ == "__main__":
    for f in sys.argv[1:]:
        main(f)
