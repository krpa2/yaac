# Sound (AMY) — AMY synthesizer test (ESP32-S3-Touch-LCD-3.49)

Test of the [AMY synthesizer](https://github.com/shorepine/amy) on this board.

With `#define PLAY_MIDI_FILES 1` (default) the firmware plays every MIDI file in `assets/` in turn
(see **MIDI files** below). Otherwise, or when `assets/` has no `.mid` files, it plays the song
selected with `#define SONG` in `main/main.c`:

- `song_korobeiniki` (default) — **Korobeiniki**, the public-domain folk song known as the Tetris
  "Type A" theme, as a 4-channel NES/Game Boy-style chiptune: 25% pulse lead, 50% pulse 16th-note
  arpeggios, triangle octave bass, sine-drop kick + noise snare/hat, light echo. Form A A B A, 144 BPM.
- `song_imperial_march_piano` — the Imperial March score from `../sound` on AMY's partials piano (patch 256).

## Layout
- `components/amy/` — AMY 1.2.187 `src/` vendored as an ESP-IDF component (commit in `VERSION`, MIT `LICENSE`).
  `CMakeLists.txt` builds the portable engine + ESP32 platform layer (`i2s.c`) only.
- `main/main.c` — board audio bring-up (I2C, TCA9554 EXIO7 speaker amp, I2S0, ES8311 via `esp_codec_dev`),
  AMY start, audio task
- `main/amy_player.c` — portable scheduler: song tracks -> timestamped AMY note events (shared with `pc_sim`)
- `main/song_korobeiniki.c` — chip synth definitions (AMY wire messages) + melody; bass/arp/drums generated from chords
- `main/song_imperial.c` — piano setup; score from `../sound/main/imperial_march.c` (shared, not copied)
- `main/midi_file.c`, `main/midi_player.c`, `main/midi_playlist.h` — MIDI file playback (see below)
- `pc_sim/` — renders the songs and MIDI files to WAV on the PC with the same AMY/player/song code

## MIDI files
Drop `.mid` files into `assets/` and rebuild: CMake embeds each one (`EMBED_FILES`) and generates a
playlist table (`build/esp-idf/main/midi_playlist.c`).

- `main/midi_file.c` — Standard MIDI File reader (format 0/1): merges all tracks, converts ticks to ms
  through the tempo map, records per-channel note counts, first program and peak polyphony.
- `main/midi_player.c` — one AMY synth per used channel (synth = channel + 1); General MIDI programs
  mapped to AMY Juno-6 / DX7 patches (`gm_map`, an approximation by instrument name); channel 10 on
  AMY's GM drum kit (patch 258, built-in tiny PCM set). Voices per channel = the channel's peak
  polyphony, capped at `MIDI_MAX_VOICES_PER_CHANNEL` (6). Handles note on/off, CC7 volume,
  CC11 expression, CC64 sustain and program changes within the same patch family; pitch bend is ignored.
- Osc pool: AMY is started once to measure how many oscs the chosen patches occupy, then restarted
  with exactly that pool (+8), so the two-core split stays balanced.
- The log prints the channel -> patch table, the osc pool and free heap for each file.

Playlist now: `chiptune_1.mid` — 6 channels (two square leads, the second an echo one 16th behind,
synth brass, saw lead, synth strings, drums), 90 BPM, 213 s, 57-osc pool.
GM programs 80 (square lead) and 81 (saw lead) play on raw AMY oscillators (25% pulse / saw) instead of
Juno patches, for a real chip sound (`chip_voices` in `midi_player.c`). MIDI files play at
`MIDI_VOLUME` 2.0 (+6 dB).

Set aside in `assets/original/` (not embedded): `topgun.mid` (13 channels, 344-osc pool) and the reduced
`topgun_4ch.mid` (126 osc). Tools: `tools/midi_info.py` (summary, duplicates, non-GM drum notes) and
`tools/midi_reduce.py` (keep/move channels, force programs, re-map drum notes).

## How it works
- AMY runs with `audio = AMY_AUDIO_IS_NONE`: it only renders; `amy_update()` returns a 256-frame
  stereo block that the audio task (core 1) writes to the codec. The blocking I2S write paces AMY's clock.
- `platform.multicore = 1`: AMY splits oscillator rendering with a helper task on core 0.
- Notes are sent as AMY events to each track's synth, timestamped
  (`e.time`) and queued 50 ms ahead of `amy_sysclock()`, so timing doesn't depend on task jitter.
- Every 5 s the log prints average and peak render time per block vs. the 5.8 ms budget.

## Performance notes (measured on this board)
| Setup | Render load |
|---|---|
| 6 voices, default `max_oscs` 250, 16 KB D-cache | peak 117–133% (over budget) |
| 4 voices, `max_oscs` 104, 64 KB D-cache / 32 KB I-cache | avg ~20%, peak ~90–108% |

- AMY splits oscs by index between the two cores (first half on the helper task), so `max_oscs`
  must be sized to the voices in use: one patch-256 voice = 1 control osc + 24 partials = 25 oscs,
  plus a few spare (AMY asserts `osc < AMY_OSCS - (partials + 1)`).
- Peaks are likely isolated blocks (e.g. note-on partial setup); with a ~20% average, the I2S DMA ring (~33 ms) should absorb them.

## Build & flash (ESP-IDF 5.5.x)
    . C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
    idf.py build
    idf.py -p COM15 flash monitor

## PC render (`pc_sim/`)
    sound_amy\pc_sim\build.bat     -> korobeiniki_chiptune.wav, imperial_march_piano.wav,
                                     plus <name>.wav for every assets\*.mid
    pc_sim\build\pc_sim.exe file.mid ...   (render specific MIDI files)

Uses `amy_simple_fill_buffer()` instead of `amy_update()` (on desktop, `amy_update()` waits for a
miniaudio playback thread). No multicore split on PC; everything else is the firmware's code.

Tweaks: `OUTPUT_VOLUME` / `AMY_VOLUME` / `SONG` in `main.c`; per-song `volume`, `bpm`, `max_oscs`,
synth patches and track velocities in `song_*.c`. Piano patch numbers: 0–127 Juno-6, 128–255 DX7, 256 piano.
