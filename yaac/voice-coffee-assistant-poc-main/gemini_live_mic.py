#!/usr/bin/env python3
"""
Gemini Live API test from the Mac microphone.

Uses the SAME raw protocol the ESP32-S3 will use (no SDK):
  - one TLS WebSocket to generativelanguage.googleapis.com
  - first JSON message "setup" (model, system prompt, tools)
  - continuous stream of 16 kHz / 16-bit / mono PCM in base64, 32 ms chunks
  - replies: 24 kHz PCM audio in base64, transcriptions, toolCall
  - toolCall -> schema-driven validation (as the firmware will do) -> simulated machine -> toolResponse

Configuration files (editable without touching the code):
  tools.json         functions exposed to the model (+ x-range constraints enforced on the device only)
  system_prompt.md   system prompt

Usage:
  put GEMINI_API_KEY=your_key in voice_coffee_assistant_poc/.env   (or export it as an environment variable)
  python gemini_live_mic.py                 # use headphones!
  python gemini_live_mic.py --half-duplex   # when using the Mac speakers
"""

import argparse
import asyncio
import base64
import copy
import json
import os
import sys
import threading
import time
from pathlib import Path

import numpy as np
import sounddevice as sd
import websockets
from dotenv import load_dotenv

HERE = Path(__file__).resolve().parent
load_dotenv(HERE / ".env")  # optional: GEMINI_API_KEY / GEMINI_LIVE_MODEL; real env vars take precedence
IN_RATE = 16000    # Live API: 16 kHz PCM input
OUT_RATE = 24000   # Live API: 24 kHz PCM output
DEFAULT_MODEL = "gemini-2.5-flash-native-audio-preview-09-2025"  # check the current name in AI Studio

WS_URL = ("wss://generativelanguage.googleapis.com/ws/"
          "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={key}")


# --------------------------------------------------------------------------- tools: loading and validation

def load_tools(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return {f["name"]: f for f in data["functionDeclarations"]}


def strip_private(obj):
    """Remove x-* and _* keys before sending the schema to Gemini."""
    if isinstance(obj, dict):
        return {k: strip_private(v) for k, v in obj.items()
                if not k.startswith("x-") and not k.startswith("_")}
    if isinstance(obj, list):
        return [strip_private(v) for v in obj]
    return obj


def validate(decl, args):
    """Generic schema-driven whitelist: this is the logic to replicate in C in the firmware.
    Returns (normalized_args, None) or (None, rejection_reason)."""
    schema = decl.get("parameters", {})
    props = schema.get("properties", {})
    args = dict(args or {})
    for k in args:
        if k not in props:
            return None, f"parameter not allowed: {k}"
    for k in schema.get("required", []):
        if k not in args:
            return None, f"missing required parameter: {k}"
    out = {}
    for k, v in args.items():
        p = props[k]
        t = p.get("type")
        if t == "STRING":
            if not isinstance(v, str):
                return None, f"{k}: string expected"
            if "enum" in p and v not in p["enum"]:
                return None, f"{k}: value not allowed '{v}'"
        elif t == "INTEGER":
            if isinstance(v, bool) or not isinstance(v, (int, float)) or float(v) != int(v):
                return None, f"{k}: integer expected"
            v = int(v)
            if "x-range" in p and not p["x-range"][0] <= v <= p["x-range"][1]:
                return None, f"{k}: out of range {p['x-range']}"
        elif t == "BOOLEAN":
            if not isinstance(v, bool):
                return None, f"{k}: boolean expected"
        out[k] = v
    return out, None


# --------------------------------------------------------------------------- simulated machine

MILK_DRINKS = {"cappuccino", "latte_macchiato", "caffe_latte", "flat_white", "hot_milk"}
NO_COFFEE = {"hot_milk", "hot_water"}
BASE_COFFEE_ML = {"ristretto": 25, "espresso": 40, "coffee": 100, "lungo": 140, "americano": 180,
                  "cappuccino": 40, "latte_macchiato": 40, "caffe_latte": 60, "flat_white": 60,
                  "hot_milk": 0, "hot_water": 200}
BASE_MILK_S = {"cappuccino": 5, "latte_macchiato": 5, "caffe_latte": 5, "flat_white": 5, "hot_milk": 5}
LENGTH_FACTOR = {"short": 0.7, "normal": 1.0, "long": 1.4, "extra_long": 1.8}
MILK_FACTOR = {"less": 0.7, "normal": 1.0, "more": 1.3}
TEMP_C = {"low": 84, "normal": 90, "high": 94}

MAINTENANCE_NEEDS = {
    "rinse": "Only a cup under the coffee spout is needed. Takes about 30 seconds.",
    "clean_milk_circuit": "A container under the milk spout and the milk tube in a jug of water are needed. About 1 minute.",
    "descale": "Philips descaler, water tank filled to the marked level, a container of at least 1.5 litres "
               "and the AquaClean filter removed are needed. About 30 minutes.",
    "replace_water_filter": "A new AquaClean filter to insert in the water tank is needed. The machine then runs a short rinse.",
}


class FakeMachine:
    """Simulates the machine controller. Levels (short/long, less/more...) are resolved into physical
    values (ml, milk seconds, degrees C) HERE, i.e. on the firmware side, never by the model."""

    BREW_SECONDS = 25

    def __init__(self, decls):
        self.decls = decls
        self.busy_until = 0.0
        self.busy_what = None
        self.settings = {d: {"strength": 3, "coffee_length": "normal", "milk_amount": "normal",
                             "temperature": "normal"} for d in BASE_COFFEE_ML}
        self.favorite = {"drink": "cappuccino", "strength": 4}
        self.last = None
        self.cups_since_descale = 412

    # -- helpers
    def _busy(self):
        return time.time() < self.busy_until

    def _uart(self, frame):
        print(f"\n    [UART -> machine] {frame}", flush=True)

    # -- functions
    def brew(self, a):
        if self._busy():
            return {"status": "busy", "reason": f"{self.busy_what} in progress"}
        drink = a["drink"]
        ignored = []
        if drink not in MILK_DRINKS and "milk_amount" in a:
            ignored.append("milk_amount")
        if drink in NO_COFFEE:
            ignored += [k for k in ("strength", "coffee_length", "extra_shot") if k in a]
        s = dict(self.settings[drink])
        s.update({k: v for k, v in a.items() if k in s and k not in ignored})
        cups = a.get("cups", 1)
        coffee_ml = round(BASE_COFFEE_ML[drink] * LENGTH_FACTOR[s["coffee_length"]])
        milk_s = round(BASE_MILK_S.get(drink, 0) * MILK_FACTOR[s["milk_amount"]])
        extra = int(a.get("extra_shot", False) and "extra_shot" not in ignored)
        self._uart(f"BREW {drink} aroma={s['strength']} coffee={coffee_ml}ml milk={milk_s}s "
                   f"temp={TEMP_C[s['temperature']]}C extra_shot={extra} cups={cups}")
        self.last = dict(a)
        self.busy_until, self.busy_what = time.time() + self.BREW_SECONDS * cups, "brewing"
        res = {"status": "started", "drink": drink, "applied": s, "cups": cups,
               "estimated_seconds": self.BREW_SECONDS * cups}
        if ignored:
            res["ignored"] = ignored
        return res

    def brew_saved(self, a):
        src = self.favorite if a["which"] == "favorite" else self.last
        if not src:
            return {"status": "rejected", "reason": "no previous drink in memory"}
        return self.brew(dict(src))

    def stop(self, _a):
        if not self._busy():
            return {"status": "nothing_to_stop"}
        self.busy_until = 0
        self._uart("STOP")
        return {"status": "stopped"}

    def get_drink_settings(self, a):
        return {"drink": a["drink"], **self.settings[a["drink"]],
                "is_favorite": self.favorite.get("drink") == a["drink"]}

    def save_drink_settings(self, a):
        changes = {k: v for k, v in a.items() if k in ("strength", "coffee_length", "milk_amount", "temperature")}
        if not a["confirmed"]:
            return {"status": "needs_confirmation", "drink": a["drink"], "will_change": changes,
                    "as_favorite": a.get("as_favorite", False)}
        self.settings[a["drink"]].update(changes)
        if a.get("as_favorite"):
            self.favorite = {"drink": a["drink"], **changes}
        self._uart(f"SAVE {a['drink']} {changes} favorite={a.get('as_favorite', False)}")
        return {"status": "saved", "drink": a["drink"], "settings": self.settings[a["drink"]]}

    def get_machine_status(self, _a):
        return {"water_tank": "ok", "beans": "ok", "drip_tray": "ok", "grounds_container": "70%",
                "aquaclean_filter": "ok", "cups_until_descale": max(0, 500 - self.cups_since_descale),
                "busy": self.busy_what if self._busy() else None}

    def start_maintenance(self, a):
        if self._busy():
            return {"status": "busy", "reason": f"{self.busy_what} in progress"}
        if not a["confirmed"]:
            return {"status": "needs_confirmation", "procedure": a["procedure"],
                    "user_must_prepare": MAINTENANCE_NEEDS[a["procedure"]]}
        self._uart(f"MAINT {a['procedure']}")
        self.busy_until, self.busy_what = time.time() + 60, a["procedure"]
        return {"status": "started", "procedure": a["procedure"]}

    # -- dispatcher
    def handle(self, name, args):
        decl = self.decls.get(name)
        if decl is None:
            return {"status": "rejected", "reason": f"unknown function: {name}"}
        clean, err = validate(decl, args)
        if err:
            return {"status": "rejected", "reason": err}
        fn = getattr(self, name, None)
        if fn is None:
            return {"status": "rejected", "reason": f"function not implemented on the device: {name}"}
        return fn(clean)


# --------------------------------------------------------------------------- playback

class Player:
    """24 kHz playback buffer, flushed on 'interrupted' (barge-in)."""

    def __init__(self):
        self.buf = bytearray()
        self.lock = threading.Lock()

    def push(self, pcm: bytes):
        with self.lock:
            self.buf.extend(pcm)

    def clear(self):
        with self.lock:
            self.buf.clear()

    def is_playing(self):
        with self.lock:
            return len(self.buf) > 0

    def callback(self, outdata, frames, _time, _status):
        need = frames * 2  # int16 mono
        with self.lock:
            chunk = bytes(self.buf[:need])
            del self.buf[:need]
        outdata[:len(chunk)] = chunk
        if len(chunk) < need:
            outdata[len(chunk):] = b"\x00" * (need - len(chunk))


# --------------------------------------------------------------------------- session

class Stats:
    def __init__(self):
        self.chunks_up = 0
        self.bytes_up = 0
        self.bytes_down = 0
        self.t0 = time.time()
        self.last_voice_t = None      # last mic chunk above threshold (~ end of speech)
        self.waiting_first_audio = True
        self.audio_chunks_down = 0
        self.audio_bytes_down = 0


async def uplink(ws, mic_q, player, stats, args):
    while True:
        pcm = await mic_q.get()
        samples = np.frombuffer(pcm, dtype=np.int16)
        rms = float(np.sqrt(np.mean(samples.astype(np.float32) ** 2))) if samples.size else 0.0

        muted = args.half_duplex and player.is_playing()
        if muted:
            pcm = b"\x00" * len(pcm)   # send silence: keep the stream continuous
        elif rms > args.voice_rms and not player.is_playing():
            # ignore the mic while the reply is playing: echo or barge-in must not move the end-of-speech mark
            stats.last_voice_t = time.time()

        msg = {"realtimeInput": {"audio": {"mimeType": f"audio/pcm;rate={IN_RATE}",
                                           "data": base64.b64encode(pcm).decode("ascii")}}}
        txt = json.dumps(msg, separators=(",", ":"))
        await ws.send(txt)
        stats.chunks_up += 1
        stats.bytes_up += len(txt)


async def downlink(ws, player, machine, stats, debug=False):
    speaker = None  # "USER" / "ASSISTANT": start a new line when the speaker changes
    seen_mimes = set()

    def say(who, text):
        nonlocal speaker
        if who != speaker:
            print(f"\n{who}: ", end="", flush=True)
            speaker = who
        print(text, end="", flush=True)

    async for raw in ws:
        stats.bytes_down += len(raw)
        msg = json.loads(raw)   # the server often sends JSON in binary frames: json.loads accepts bytes
        if debug:
            keys = {k: (list(v.keys()) if isinstance(v, dict) else type(v).__name__) for k, v in msg.items()}
            print(f"\n    [debug] {keys}", flush=True)

        if "serverContent" in msg:
            sc = msg["serverContent"]
            if "inputTranscription" in sc:
                say("USER", sc["inputTranscription"].get("text", ""))
            if "outputTranscription" in sc:
                say("ASSISTANT", sc["outputTranscription"].get("text", ""))
            for part in sc.get("modelTurn", {}).get("parts", []):
                inline = part.get("inlineData")
                if inline and inline.get("mimeType") not in seen_mimes:
                    seen_mimes.add(inline.get("mimeType"))
                    print(f"\n    [downlink audio format: {inline.get('mimeType')}]", flush=True)
                if inline and inline.get("mimeType", "").startswith("audio/pcm"):
                    stats.audio_chunks_down += 1
                    stats.audio_bytes_down += len(inline["data"]) * 3 // 4
                    if stats.waiting_first_audio and stats.last_voice_t:
                        lat = (time.time() - stats.last_voice_t) * 1000
                        print(f"\n    [latency ~{lat:.0f} ms: last voice at mic -> first reply audio]", flush=True)
                        stats.waiting_first_audio = False
                    player.push(base64.b64decode(inline["data"]))
            if sc.get("interrupted"):
                player.clear()
                print("\n    [interrupted by user: speaker buffer flushed]", flush=True)
            if sc.get("turnComplete"):
                stats.waiting_first_audio = True   # arm the latency measurement once per turn
                speaker = None
                print("", flush=True)

        elif "toolCall" in msg:
            responses = []
            for fc in msg["toolCall"].get("functionCalls", []):
                result = machine.handle(fc.get("name"), fc.get("args"))
                print(f"\n    [toolCall] {fc.get('name')}({json.dumps(fc.get('args', {}), ensure_ascii=False)})"
                      f"\n    [response] {json.dumps(result, ensure_ascii=False)}", flush=True)
                responses.append({"id": fc.get("id"), "name": fc.get("name"), "response": result})
            await ws.send(json.dumps({"toolResponse": {"functionResponses": responses}}))

        elif "toolCallCancellation" in msg:
            print(f"\n    [toolCall cancelled: {msg['toolCallCancellation'].get('ids')}]", flush=True)

        elif "goAway" in msg:
            print(f"\n    [goAway: server will close in {msg['goAway'].get('timeLeft')}]", flush=True)


async def run(args):
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        sys.exit("GEMINI_API_KEY is not set: add it to .env next to this script, or export it")

    decls = load_tools(args.tools)
    system_prompt = Path(args.prompt).read_text(encoding="utf-8")
    loop = asyncio.get_running_loop()
    mic_q: asyncio.Queue = asyncio.Queue()
    player, machine, stats = Player(), FakeMachine(decls), Stats()
    chunk_frames = IN_RATE * args.chunk_ms // 1000

    def mic_cb(indata, _frames, _time, status):
        if status:
            print(f"\n    [mic: {status}]", file=sys.stderr)
        loop.call_soon_threadsafe(mic_q.put_nowait, bytes(indata))

    setup = {"setup": {
        "model": f"models/{args.model}",
        "generationConfig": {"responseModalities": ["AUDIO"]},
        "systemInstruction": {"parts": [{"text": system_prompt}]},
        "tools": [{"functionDeclarations": strip_private(list(copy.deepcopy(decls).values()))}],
        "inputAudioTranscription": {},
        "outputAudioTranscription": {},
    }}

    t_conn = time.time()
    async with websockets.connect(WS_URL.format(key=key), max_size=None, ping_interval=20) as ws:
        await ws.send(json.dumps(setup))
        first = json.loads(await ws.recv())
        if "setupComplete" not in first:
            sys.exit(f"Setup failed: {first}")
        print(f"Session ready in {(time.time() - t_conn) * 1000:.0f} ms (TLS + setup). "
              f"{len(decls)} functions: {', '.join(decls)}.\n"
              f"Chunk: {args.chunk_ms} ms = {chunk_frames * 2} bytes of PCM. Start talking, Ctrl+C to quit.")
        if not args.half_duplex:
            print("Full-duplex mode: use HEADPHONES, otherwise the model hears itself and interrupts itself.")

        in_dev = args.in_device if args.in_device is not None else sd.default.device[0]
        out_dev = args.out_device if args.out_device is not None else sd.default.device[1]
        print(f"Input device:  {sd.query_devices(in_dev)['name']}\n"
              f"Output device: {sd.query_devices(out_dev)['name']}  (change with --out-device, see --list-devices)")
        if sd.query_devices(in_dev)["name"] == sd.query_devices(out_dev)["name"]:
            print("WARNING: input and output are the same device. With Bluetooth headsets, opening the mic "
                  "switches them to call mode and the output may go silent: use the Mac built-in mic "
                  "(--in-device) and keep the headset for output only.")

        with sd.RawInputStream(samplerate=IN_RATE, channels=1, dtype="int16", device=args.in_device,
                               blocksize=chunk_frames, callback=mic_cb), \
             sd.RawOutputStream(samplerate=OUT_RATE, channels=1, dtype="int16", device=args.out_device,
                                callback=player.callback):
            if args.test_tone:
                t = np.arange(int(OUT_RATE * 0.5)) / OUT_RATE
                player.push((np.sin(2 * np.pi * 880 * t) * 8000).astype(np.int16).tobytes())
                print("Playing a 0.5 s test tone on the output device...")
            try:
                await asyncio.gather(uplink(ws, mic_q, player, stats, args),
                                     downlink(ws, player, machine, stats, args.debug))
            except websockets.ConnectionClosed as e:
                print(f"\nConnection closed by server: code={e.code} reason={e.reason!r}")
            finally:
                dt = time.time() - stats.t0
                print(f"\n--- {dt:.0f} s, {stats.chunks_up} chunks sent, "
                      f"average uplink {stats.bytes_up * 8 / dt / 1000:.0f} kbit/s, "
                      f"average downlink {stats.bytes_down * 8 / dt / 1000:.0f} kbit/s, "
                      f"{stats.audio_chunks_down} audio chunks received "
                      f"({stats.audio_bytes_down / (OUT_RATE * 2):.1f} s of reply audio)")


def main():
    p = argparse.ArgumentParser(description="Gemini Live test from the Mac mic (raw protocol, same as the ESP32-S3)")
    p.add_argument("--model", default=os.environ.get("GEMINI_LIVE_MODEL", DEFAULT_MODEL))
    p.add_argument("--tools", default=str(HERE / "tools.json"), help="JSON file with the functionDeclarations")
    p.add_argument("--prompt", default=str(HERE / "system_prompt.md"), help="system prompt file")
    p.add_argument("--chunk-ms", type=int, default=32, help="duration of each audio chunk (default 32 ms = ESP-SR AFE block)")
    p.add_argument("--half-duplex", action="store_true", help="send silence while the assistant speaks (no headphones)")
    p.add_argument("--voice-rms", type=float, default=600.0, help="RMS threshold to estimate end of speech (latency measurement)")
    p.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    p.add_argument("--in-device", type=int, default=None, help="input device index (see --list-devices)")
    p.add_argument("--out-device", type=int, default=None, help="output device index (see --list-devices)")
    p.add_argument("--test-tone", action="store_true", help="play a short beep at start to check the speaker")
    p.add_argument("--debug", action="store_true", help="print the keys of every server message")
    args = p.parse_args()
    if args.list_devices:
        print(sd.query_devices())
        return
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nBye.")


if __name__ == "__main__":
    main()
