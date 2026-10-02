# Gemini Live test from the Mac microphone

Script that talks to the Gemini Live API using the same raw protocol planned for the ESP32-S3:
WebSocket + JSON, 16 kHz PCM in base64 in 32 ms chunks, 24 kHz PCM reply, function calling
validated against a whitelist on a simulated machine.

## 1. Check the audio first

Before talking to Gemini, make sure the Mac can record and play at the same time
(after the setup below):

```fish
python audio_diag.py
```

It plays three beeps:

| Beep | Configuration |
|------|---------------|
| A (low)  | 24 kHz output stream with callback, mic closed (same as the script) |
| B (mid)  | same, with the 16 kHz mic stream open at the same time |
| C (high) | output at the device native rate, mic open |

- **A, B and C all audible**: audio is fine, go ahead.
- **Only A audible**: opening the mic silences the output. Typical with Bluetooth headsets/AirPods
  used as both mic and output: opening the mic switches them to call mode (HFP). Fix: use the Mac
  built-in mic and keep the headset for output only
  (`python gemini_live_mic.py --list-devices`, then `--in-device <n>`, or set the input in
  System Settings → Sound → Input), or use wired headphones.
- **A not audible**: wrong output device. Check System Settings → Sound → Output or use `--out-device <n>`.

The main script also prints the input/output devices at start and warns when they are the same device.
`--test-tone` plays a beep inside the real session.

## 2. Setup (fish)

```fish
cd voice_coffee_assistant_poc
python3 -m venv .venv
source .venv/bin/activate.fish
pip install -r requirements.txt
cp .env.example .env                    # then put your key in .env (from aistudio.google.com)
python gemini_live_mic.py
```

The key is read from `.env` next to the script (via python-dotenv). An exported `GEMINI_API_KEY`
environment variable takes precedence. `.env` is git-ignored: never commit or share it.

On first run macOS asks for microphone permission for the Terminal.

## 3. Options

- `--half-duplex` if you use the Mac speakers instead of headphones (the mic sends silence while the assistant speaks)
- `--model <name>` if the default model no longer exists: take the current Live "native audio" model name from AI Studio
- `--chunk-ms 64` to try larger chunks
- `--tools <file>` / `--prompt <file>` to use alternative function sets or prompts
- `--list-devices`, `--in-device <n>`, `--out-device <n>` to pick the audio devices
- `--test-tone` plays a short beep at start to check the speaker; `--debug` prints the keys of every server message

## 4. Functions and prompt

- `tools.json`: functions exposed to the model. Descriptions are the "prompt" that drives parameter selection.
  `x-range` keys are not sent to Gemini: they are constraints enforced by the device only.
- `system_prompt.md`: assistant behaviour.

Edit the two files and rerun: no code changes needed. Validation in `validate()` is generic over the schema
and is the logic to replicate in C in the firmware.

## 5. What to try (speak Italian or English)

1. "Make me a strong cappuccino" → see `toolCall`, the simulated UART frame and the spoken confirmation.
2. Ask for a second coffee right after → the machine answers `busy`, the model must tell you.
3. "A long, very hot coffee" / "a short espresso" / "latte macchiato with less milk" → levels resolved into ml/°C in the UART frame.
4. "Stop!" during brewing → immediate `stop`.
5. "From now on make my coffee longer" → `save_drink_settings` with two-step confirmation.
6. "Make another one" / "the usual" → `brew_saved`.
7. "Do I need to descale?" then "ok, let's do it" → status + maintenance with confirmation.
8. "Make me a double" → it must ask: two cups or stronger?
9. "What is the difference between a flat white and a cappuccino?" → expert answer without tools.
10. Interrupt it while it speaks (with headphones) → `[interrupted]`, the speaker buffer is flushed.
11. Run the grinder next to the Mac while speaking → how well the server VAD copes with the noise.

The script prints: session open time (TLS + setup), estimated latency from end of speech to first reply audio,
and average up/down bandwidth. These are the figures to compare later with the ESP32-S3.
