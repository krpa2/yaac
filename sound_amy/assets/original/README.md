# Source MIDI files (not embedded)

Only `.mid` files directly in `assets/` go into the firmware. Originals live here.

`topgun_4ch.mid` was made from `topgun.mid` with (moved out of the playlist):

    python tools/midi_reduce.py assets/original/topgun.mid assets/topgun_4ch.mid \
        --keep 2,4,6,10 --move 9:10 --drum-map 20:36,21:38,31:39,32:37,33:49

- ch 4 `gtr 1` lead (ch 12/13 were exact duplicates of it), ch 2 `brass pad`, ch 6 `bass`, ch 10 drums
- the `CYMBALS` track was on ch 9 with an organ program -> moved to drum channel 10
- non-GM drum notes re-mapped by rhythmic role (guesses): 20 kick, 21 snare, 31 clap, 32 side stick, 33 crash
