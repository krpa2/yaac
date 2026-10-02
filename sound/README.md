# Sound — Imperial March on a synth piano (ESP32-S3-Touch-LCD-3.49)

Plays the main theme of "The Imperial March" (G minor, ♩=104) in a loop through the
onboard ES8311 codec and speaker header. No audio files: the notes are synthesized live.

## Files
- `main/main.c` — I2C bus, speaker amp enable (TCA9554 EXIO7), I2S0 + ES8311 via `esp_codec_dev`
- `main/piano_synth.c` — 12-voice additive "synth piano": 6 slightly inharmonic partials with
  per-partial exponential decay + a detuned second string, 3 ms attack, 120 ms release, linear mix (`MASTER_GAIN`)
- `main/sequencer.c` — portable sample-accurate sequencer (shared with `pc_sim`)
- `main/imperial_march.c` — the score: right-hand melody + left-hand bass (edit here to change the tune)

## Audio wiring
| Signal | GPIO |
|---|---|
| I2C SDA / SCL (ES8311 @0x18, TCA9554 @0x20) | 47 / 48 |
| I2S MCLK / BCLK / WS / DOUT | 7 / 15 / 46 / 45 |
| Speaker amp enable | TCA9554 EXIO7 (high = on) |

## Build & flash (ESP-IDF 5.5.x)
    . C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
    idf.py build
    idf.py -p COM15 flash monitor

Tweaks: `OUTPUT_VOLUME` in `main.c`, `imperial_march_bpm` / track velocities in `imperial_march.c`,
`partial_level` / `MASTER_GAIN` in `piano_synth.c` for tone and loudness. Bass is written an octave up (octave 3) because the small speaker cannot reproduce ~100 Hz.

## PC simulation (`pc_sim/`)
Compiles the same `piano_synth.c`, `sequencer.c` and `imperial_march.c` with MSVC and writes WAVs:

    pc_simuild.bat [output_prefix] [speaker_resonance_hz] [speaker_q]     (defaults: imperial_march 600 1.2)

- `imperial_march_dac.wav` — the signal the firmware feeds the ES8311 (volume 100 = no extra digital gain)
- `imperial_march_small_speaker.wav` — same signal through a crude micro-speaker model
  (2nd-order high-pass at the speaker resonance + 8 kHz roll-off)

If the `_dac` file sounds clean on PC speakers/headphones but the board sounds harsh, the cause is
the onboard amp/speaker, not the synth.
