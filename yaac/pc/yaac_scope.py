#!/usr/bin/env python3
"""YAAC voice stream oscilloscope.

Receives the UDP broadcast audio that the YAAC board sends on the local network
after its wake word and draws it as an oscilloscope. Standard library only (tkinter).

    python yaac_scope.py [--window 2.0] [--save recordings] [--bind 192.168.0.20]

Packet format (little endian, see yaac/main/net_stream.h):
    char[4] "YAAC", u8 version, u8 flags (1=start, 2=end), u16 samples,
    u32 session, u32 seq, u32 sample_rate, i16[samples]
"""
import argparse
import array
import math
import os
import socket
import struct
import threading
import time
import tkinter as tk
import wave
from datetime import datetime

PORT = 5577
HEADER = struct.Struct("<4sBBHIII")
FLAG_START, FLAG_END = 0x01, 0x02

BG, GRID, AXIS, TRACE, TEXT, LIVE, ENDED = (
    "#0b0f14", "#1c2530", "#33414f", "#50dcff", "#c8ccd4", "#3cff78", "#ffc828")


class Receiver(threading.Thread):
    """Receives the board's broadcast packets and collects samples of the current session."""

    def __init__(self, bind_ip, port, save_dir, max_samples):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.samples = array.array("h")
        self.max_samples = max_samples
        self.session = None
        self.rate = 16000
        self.live = False
        self.packets = self.lost = self.bad = 0
        self.last_seq = None
        self.last_rx = 0.0
        self.peak = 0
        self.save_dir = save_dir
        self.wav = None
        self.wav_name = ""

        self.sender = ""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind((bind_ip, port))      # "" = all interfaces: receives subnet broadcasts
        self.sock.settimeout(0.5)

    def _open_wav(self):
        if not self.save_dir:
            return
        os.makedirs(self.save_dir, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.wav_name = os.path.join(self.save_dir, f"yaac_{stamp}_s{self.session}.wav")
        self.wav = wave.open(self.wav_name, "wb")
        self.wav.setnchannels(1)
        self.wav.setsampwidth(2)
        self.wav.setframerate(self.rate)

    def _close_wav(self):
        if self.wav:
            self.wav.close()
            print(f"saved {self.wav_name}")
            self.wav = None

    def run(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            if len(data) < HEADER.size:
                self.bad += 1
                continue
            magic, version, flags, n, session, seq, rate = HEADER.unpack_from(data)
            if magic != b"YAAC" or version != 1 or len(data) < HEADER.size + 2 * n:
                self.bad += 1
                continue
            pcm = array.array("h")
            pcm.frombytes(data[HEADER.size:HEADER.size + 2 * n])
            with self.lock:
                self.sender = addr[0]
                if flags & FLAG_START or session != self.session:
                    self._close_wav()
                    self.session, self.rate = session, rate
                    self.samples = array.array("h")
                    self.packets = self.lost = 0
                    self.last_seq = None
                    self.peak = 0
                    self.live = True
                    self._open_wav()
                if self.last_seq is not None and seq > self.last_seq + 1:
                    self.lost += seq - self.last_seq - 1
                self.last_seq = seq
                self.packets += 1
                self.last_rx = time.time()
                if n:
                    self.samples.extend(pcm)
                    if len(self.samples) > self.max_samples:
                        del self.samples[:len(self.samples) - self.max_samples]
                    self.peak = max(self.peak, max(abs(s) for s in pcm))
                    if self.wav:
                        self.wav.writeframes(pcm.tobytes())
                if flags & FLAG_END:
                    self.live = False
                    self._close_wav()

    def snapshot(self, count):
        with self.lock:
            return (self.samples[-count:], self.session, self.rate, self.live,
                    self.packets, self.lost, self.peak, self.last_rx, self.sender)


class Scope:
    def __init__(self, rx, window_s, gain):
        self.rx, self.window_s, self.gain = rx, window_s, gain
        self.root = tk.Tk()
        self.root.title("YAAC voice stream")
        self.root.configure(bg=BG)
        self.canvas = tk.Canvas(self.root, width=1100, height=420, bg=BG, highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.status = tk.Label(self.root, anchor="w", bg=BG, fg=TEXT, font=("Consolas", 11))
        self.status.pack(fill=tk.X, padx=8, pady=4)
        self.root.bind("<Up>", lambda e: self._zoom(2))
        self.root.bind("<Down>", lambda e: self._zoom(0.5))
        self.root.bind("<Escape>", lambda e: self.root.destroy())
        self._tick()

    def _zoom(self, f):
        self.gain = min(64, max(1, self.gain * f))

    def _tick(self):
        self._draw()
        self.root.after(50, self._tick)

    def _draw(self):
        c = self.canvas
        w, h = max(c.winfo_width(), 200), max(c.winfo_height(), 100)
        mid, half = h // 2, h // 2 - 12
        c.delete("all")

        rate = self.rx.rate or 16000
        count = int(self.window_s * rate)
        samples, session, rate, live, packets, lost, peak, last_rx, sender = self.rx.snapshot(count)

        # Grid: 100 ms per vertical line, quarter-scale horizontal lines
        for i in range(int(self.window_s * 10) + 1):
            x = i * w / (self.window_s * 10)
            c.create_line(x, 0, x, h, fill=GRID)
        for f in (0.25, 0.5, 0.75):
            for y in (mid - half * f, mid + half * f):
                c.create_line(0, y, w, y, fill=GRID)
        c.create_line(0, mid, w, mid, fill=AXIS)

        # Min/max envelope per pixel column, newest audio at the right edge
        n = len(samples)
        if n:
            scale = self.gain * half / 32768.0
            per_px = count / w
            start = count - n          # left part stays empty until the window fills
            pts_top, pts_bot = [], []
            for x in range(w):
                a = int(x * per_px) - start
                b = int((x + 1) * per_px) - start
                if b <= 0 or a >= n:
                    continue
                seg = samples[max(a, 0):max(min(b, n), max(a, 0) + 1)]
                hi = max(seg) * scale
                lo = min(seg) * scale
                pts_top += (x, mid - min(hi, half))
                pts_bot += (x, mid - max(lo, -half))
            if len(pts_top) >= 4:
                c.create_line(*pts_top, fill=TRACE)
                c.create_line(*pts_bot, fill=TRACE)
                for i in range(0, len(pts_top), 2):
                    c.create_line(pts_top[i], pts_top[i + 1], pts_bot[i], pts_bot[i + 1], fill=TRACE)

        c.create_text(10, 10, anchor="nw", fill=TEXT, font=("Consolas", 10),
                      text=f"{self.window_s:g} s window   100 ms/div   x{self.gain:g} (Up/Down)")

        if session is None:
            state, color = "waiting for the wake word...", TEXT
        elif live:
            state, color = f"session {session}  LIVE", LIVE
        else:
            state, color = f"session {session}  ended", ENDED
        rms = math.sqrt(sum(s * s for s in samples[-1600:]) / max(1, min(n, 1600))) if n else 0
        level = 20 * math.log10(rms / 32768 + 1e-9)
        age = f"{time.time() - last_rx:.1f} s ago" if last_rx else "-"
        self.status.configure(
            fg=color,
            text=f"UDP :{PORT} from {sender or '-'}   {state}   packets {packets}  lost {lost}   "
                 f"{n / rate:.2f} s audio   level {level:6.1f} dBFS   peak {20 * math.log10(peak / 32768 + 1e-9):6.1f} dBFS   "
                 f"last packet {age}")

    def run(self):
        self.root.mainloop()


def main():
    ap = argparse.ArgumentParser(description="YAAC voice stream oscilloscope")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--bind", default="",
                    help="local IP to listen on (default: all network cards)")
    ap.add_argument("--window", type=float, default=2.0, help="seconds shown on screen")
    ap.add_argument("--gain", type=float, default=1.0, help="vertical zoom (the board applies AGC)")
    ap.add_argument("--save", metavar="DIR", help="write each session to DIR/yaac_<time>_s<N>.wav")
    a = ap.parse_args()

    rx = Receiver(a.bind, a.port, a.save, int(a.window * 16000 * 4))
    rx.start()
    print(f"Listening for YAAC broadcasts on UDP port {a.port} ({a.bind or 'all interfaces'}). "
          f"Say the wake word to the board.")
    Scope(rx, a.window, a.gain).run()


if __name__ == "__main__":
    main()
