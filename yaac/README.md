# YAAC — Yet Another Alexa Clone (ESP32-S3-Touch-LCD-3.49)

Offline voice assistant built on Espressif **ESP-SR** (v2.5.5). Based on `../microphone`.

## Stage 1 (this version): wake word
- Say **"Alexa"**: the board plays a two-tone chime and the screen switches to **LISTENING...**
  until you stop talking (1.2 s of silence) or 30 s pass (hard limit).
- Screen: title, state, live trace of the *processed* audio (after echo cancellation / speech
  enhancement / noise suppression), level meter in dBFS, `VOICE` when VAD hears speech, wake counter.

## Audio pipeline
```
ES7210 TDM slots:  0 MIC1 | 1 speaker echo reference | 2 MIC2 | 3 unused     -> AFE input "MRMN"
ESP-SR AFE (AFE_TYPE_SR, HIGH_PERF):
    AEC  - acoustic echo cancellation against the reference (the chime is cancelled, so
           it does not re-trigger the wake word, and you can talk over speaker audio)
    SE   - 2-mic speech enhancement (beamforming)
    NS   - noise suppression (WebRTC)
    VAD  - voice activity detection (WebRTC)
    WakeNet9 "alexa" (wn9_alexa, 284 KB)
```
- The echo reference is the board's own hardware loopback: the ES7210's 3rd input is wired to
  the speaker amp output through the "AEC" network, so it carries exactly what the speaker plays.
- I2S0 runs full duplex in TDM (4 × 16-bit slots, 16 kHz) for both ES7210 (in) and ES8311 (out);
  they share BCLK/WS, so playback also writes 4-slot frames (`audio_play()` fills every slot).

## Code
- `main/main.c` — AFE setup (`afe_config_init("MRMN", ...)`), feed task (core 0), fetch task
  (core 1: wake word, VAD, listening state), chime task, UI task (~15 fps).
- `main/audio_io.c` — I2S0 TDM full duplex, ES7210 + ES8311 via `esp_codec_dev`, speaker amp.
- `main/display.c`, `main/scope_draw.c` — panel + drawing (from `../microphone`).
- `../common/board_power` — battery power latch + long-press PWR (1.5 s) off.
- `partitions.csv` — 6 MB app + **8 MB `model` partition** (ESP-SR `srmodels.bin`, built and
  flashed automatically by the esp-sr component; room for an English MultiNet model later).

## Voice streaming over Wi-Fi
- Wi-Fi station, credentials in `main/wifi_credentials.h` (**git-ignored**; template
  `main/wifi_credentials.example.h`). Reconnects automatically; power save off for steady streaming.
- After the wake word, the processed audio (AEC + 2-mic SE, 16 kHz mono 16-bit) is sent as
  **UDP broadcast on the local subnet, port 5577** (e.g. 192.168.0.255 for a /24, computed from the
  board's IP and netmask; routers do not forward it) until YAAC returns to idle.
  The stream starts with up to 1 s of pre-roll, so it contains the wake word itself.
- Packet: 20-byte header (`"YAAC"`, version, flags start/end, sample count, session, sequence,
  sample rate) + up to 512 samples (32 ms). Format in `main/net_stream.h`.
- The screen shows the IP (`WIFI 192.168.x.y`) and `STREAMING UDP` while sending.

### PC viewer (`pc/`)
    pc\yaac_scope.bat                       # or: py -3 pc\yaac_scope.py
    pc\yaac_scope.bat --save recordings     # also write each session to a WAV file
    pc\yaac_scope.bat --window 4 --bind 192.168.0.129

`yaac_scope.py` is standard-library Python (tkinter): it listens on UDP port 5577, draws a
min/max envelope of the last `--window` seconds (100 ms/div), Up/Down changes the zoom, and the
status bar shows session, packets, lost packets (sequence gaps), level and peak.
Windows asks once to allow Python through the firewall — allow it on **private** networks,
otherwise no packets arrive. The PC must be on the same subnet as the board (here 192.168.0.x).

## Build & flash (ESP-IDF 5.5.x)
    . C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
    idf.py build
    idf.py -p COM15 flash monitor          # flashes app + srmodels.bin

## Next stages (ideas)
- Stage 2: offline commands with MultiNet (English `mn7_en`), e.g. "turn on the light", "what time is it".
- Stage 3: answers — tones/screen now; spoken replies need a TTS (ESP-SR TTS is Chinese only) or a cloud service.
