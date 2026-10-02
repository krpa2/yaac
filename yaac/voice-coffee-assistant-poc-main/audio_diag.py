#!/usr/bin/env python3
"""Audio output diagnostics: plays three beeps in different configurations to find out which one is silent."""

import time

import numpy as np
import sounddevice as sd

OUT_RATE, IN_RATE = 24000, 16000


def tone(rate, freq, secs=0.8):
    t = np.arange(int(rate * secs)) / rate
    return (np.sin(2 * np.pi * freq * t) * 8000).astype(np.int16).tobytes()


def play_raw_callback(rate, freq, with_mic=False):
    buf = bytearray(tone(rate, freq))
    statuses = []

    def cb(outdata, frames, _t, status):
        if status:
            statuses.append(str(status))
        need = frames * 2
        chunk = bytes(buf[:need])
        del buf[:need]
        outdata[:len(chunk)] = chunk
        if len(chunk) < need:
            outdata[len(chunk):] = b"\x00" * (need - len(chunk))

    out = sd.RawOutputStream(samplerate=rate, channels=1, dtype="int16", callback=cb)
    mic = sd.RawInputStream(samplerate=IN_RATE, channels=1, dtype="int16", blocksize=512,
                            callback=lambda *a: None) if with_mic else None
    with out:
        if mic:
            mic.start()
        time.sleep(1.2)
        if mic:
            mic.stop()
            mic.close()
    return statuses


def main():
    in_dev, out_dev = sd.default.device
    out_info = sd.query_devices(out_dev)
    print(f"Default input:  {sd.query_devices(in_dev)['name']}")
    print(f"Default output: {out_info['name']} (native rate {out_info['default_samplerate']:.0f} Hz)\n")

    print("A) LOW beep  - 24 kHz output stream with callback, mic closed")
    print("   statuses:", play_raw_callback(OUT_RATE, 440) or "none")
    time.sleep(0.5)
    print("B) MID beep  - same, with the 16 kHz mic stream open at the same time")
    print("   statuses:", play_raw_callback(OUT_RATE, 660, with_mic=True) or "none")
    time.sleep(0.5)
    native = int(out_info["default_samplerate"])
    print(f"C) HIGH beep - output at the device native rate ({native} Hz), mic open")
    buf_rate = native
    print("   statuses:", play_raw_callback(buf_rate, 990, with_mic=True) or "none")
    print("\nWhich beeps did you hear: A, B, C?")


if __name__ == "__main__":
    main()
