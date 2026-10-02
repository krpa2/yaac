# Microphone — two-channel oscilloscope (ESP32-S3-Touch-LCD-3.49)

Captures both onboard microphones and draws them as oscilloscope traces on the 640×172 screen.

## What it shows
- Top pane: **MIC1** (green), bottom pane: **MIC2** (yellow), each with its RMS level in dBFS.
- 640 samples = 40 ms on screen, grid at **4 ms/div**, display zoom ×8 (`TRACE_GAIN`).
- Rising-edge trigger on MIC1 at `TRIG_LEVEL` keeps periodic sounds (whistle, tone) steady;
  status shows `TRIG MIC1`, or `AUTO` (free-running) when nothing crosses the level.
- Frame rate in the bottom-right corner.

## Hardware path
- **ES7210** 4-channel ADC (I2C 0x40 on GPIO47/48), TDM mode, 16 kHz / 16-bit, PGA `MIC_GAIN_DB` 30 dB.
  Selecting all four inputs puts it in TDM (same as Waveshare's `codec_board` setup).
  TDM slot order, measured on this board: 0 = MIC1, 1 = speaker echo reference (AEC network from the
  amp output; a flat ~-58 dBFS while the amp is off), 2 = MIC2, 3 = unused (~-88 dBFS).
- I2S0 RX: MCLK 7, BCLK 15, WS 46, **DIN 6**. The ES8311 / speaker amp are not used (amp held off).
- Serial log prints the RMS of all four channels once a second, so the channel order can be checked:
  ch0 and ch2 follow room sound (the two mics), ch1 stays flat, ch3 is silent.

## Code
- `main/main.c` — I2C, power latch, ES7210 + I2S TDM, audio task (core 1) filling per-mic ring
  buffers, scope task (core 0): trigger, traces, labels.
- `main/display.c` — AXS15231B QSPI panel bring-up (from `../hello_world`). No LVGL: the app draws
  straight into a PSRAM framebuffer kept in the panel's native portrait order, so frames are sent
  (10 DMA bands of 64 rows) without a rotation pass.
- `main/scope_draw.c` — landscape→panel pixel mapping, lines, rectangles, 5×7 font.
- Uses `../common/board_power`: battery power latch + long-press PWR (1.5 s) power off.

## Build & flash (ESP-IDF 5.5.x)
    . C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
    idf.py build
    idf.py -p COM15 flash monitor

Tweaks in `main.c`: `MIC_GAIN_DB`, `TRACE_GAIN`, `TRIG_LEVEL`, `SAMPLE_RATE`, `CH_MIC1` / `CH_MIC2`.
