#!/usr/bin/env python3
"""Devbox side: turn a lamp command into DTMF audio for the dongle to hear.

Stdlib only, so it runs on a locked-down Windows devbox with plain Python.
On Windows it plays through the default output device (the RDP-redirected one);
elsewhere, or with --wav, it writes a WAV file.

    python ks_tx.py rgb ff8800
    python ks_tx.py rgb 00ffcc --brightness 40
    python ks_tx.py on | off
    python ks_tx.py effect purple-breathing --speed 40
    python ks_tx.py white 200
    python ks_tx.py rgb ff0000 --wav red.wav
"""
import argparse
import io
import math
import struct
import sys
import wave

RATE = 48000
TONE_MS = 50
GAP_MS = 30
LEAD_MS = 250      # non-DTMF lead-in tone: wakes the audio path, decoder ignores it
LEAD_HZ = 440.0
TAIL_MS = 250      # silence that closes the frame (decoder needs >= 150 ms)
REPEAT_GAP_MS = 300
TONE_AMP = 0.3     # per tone; two tones peak at 0.6 (about -4.4 dBFS)
RAMP_MS = 3

VERSION = 0x5
OP_POWER, OP_RGB, OP_EFFECT, OP_WHITE = 1, 2, 3, 4

# KS03~ native effects (ks_light/protocol.py upstream).
EFFECTS = {
    "seven-color-fade": 0x82, "rgb-fade": 0x83, "red-breathing": 0x84,
    "green-breathing": 0x85, "blue-breathing": 0x86, "yellow-breathing": 0x87,
    "cyan-breathing": 0x88, "purple-breathing": 0x89, "white-breathing": 0x8A,
}

ROWS = (697.0, 770.0, 852.0, 941.0)
COLS = (1209.0, 1336.0, 1477.0, 1633.0)
# nibble -> (row, col) on the keypad; '*' = 0xE, '#' = 0xF
KEYPOS = {
    0x1: (0, 0), 0x2: (0, 1), 0x3: (0, 2), 0xA: (0, 3),
    0x4: (1, 0), 0x5: (1, 1), 0x6: (1, 2), 0xB: (1, 3),
    0x7: (2, 0), 0x8: (2, 1), 0x9: (2, 2), 0xC: (2, 3),
    0xE: (3, 0), 0x0: (3, 1), 0xF: (3, 2), 0xD: (3, 3),
}


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_frame(op, args):
    for a in args:
        if not 0 <= a <= 255:
            raise ValueError("argument out of byte range: %r" % a)
    body = bytes([(VERSION << 4) | op, *args])
    return body + bytes([crc8(body)])


def to_nibbles(frame):
    out = []
    for b in frame:
        out += [b >> 4, b & 0x0F]
    return out


def _samples(ms):
    return int(RATE * ms / 1000)


def _tone(freqs, ms, amp):
    n = _samples(ms)
    ramp = _samples(RAMP_MS)
    out = []
    for i in range(n):
        env = 1.0
        if i < ramp:
            env = 0.5 - 0.5 * math.cos(math.pi * i / ramp)
        elif i >= n - ramp:
            env = 0.5 - 0.5 * math.cos(math.pi * (n - 1 - i) / ramp)
        t = i / RATE
        out.append(env * amp * sum(math.sin(2 * math.pi * f * t) for f in freqs))
    return out


def render(frame, repeat=2):
    """Mono float samples for one frame sent `repeat` times."""
    audio = _tone((LEAD_HZ,), LEAD_MS, 0.15) + [0.0] * _samples(60)
    for k in range(repeat):
        for nib in to_nibbles(frame):
            r, c = KEYPOS[nib]
            audio += _tone((ROWS[r], COLS[c]), TONE_MS, TONE_AMP)
            audio += [0.0] * _samples(GAP_MS)
        audio += [0.0] * _samples(REPEAT_GAP_MS if k < repeat - 1 else TAIL_MS)
    return audio


def to_wav_bytes(mono):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for v in mono:
            s = max(-32767, min(32767, int(round(v * 32767))))
            frames += struct.pack("<hh", s, s)
        w.writeframes(bytes(frames))
    return buf.getvalue()


def parse_hex_color(text):
    text = text.lstrip("#")
    if len(text) != 6:
        raise argparse.ArgumentTypeError("color must be 6 hex digits, like ff8800")
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def pct(text):
    v = int(text)
    if not 0 <= v <= 100:
        raise argparse.ArgumentTypeError("must be 0..100")
    return v


def command_frame(ns):
    if ns.cmd in ("on", "off"):
        return build_frame(OP_POWER, [1 if ns.cmd == "on" else 0])
    if ns.cmd == "rgb":
        r, g, b = ns.color
        return build_frame(OP_RGB, [r, g, b, ns.brightness])
    if ns.cmd == "effect":
        if ns.brightness < 1:
            raise SystemExit("effect brightness must be 1..100")
        return build_frame(OP_EFFECT, [EFFECTS[ns.name], ns.speed, ns.brightness])
    if ns.cmd == "white":
        return build_frame(OP_WHITE, [ns.level])
    raise SystemExit("unknown command")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wav", help="write audio here instead of playing it")
    ap.add_argument("--repeat", type=int, default=2, help="send the frame N times (default 2)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("on")
    sub.add_parser("off")
    p = sub.add_parser("rgb")
    p.add_argument("color", type=parse_hex_color)
    p.add_argument("--brightness", type=pct, default=100)
    p = sub.add_parser("effect")
    p.add_argument("name", choices=sorted(EFFECTS))
    p.add_argument("--speed", type=pct, default=50)
    p.add_argument("--brightness", type=pct, default=100)
    p = sub.add_parser("white", help="KS03~ white channel, 0..255")
    p.add_argument("level", type=int, choices=range(256), metavar="LEVEL")
    ns = ap.parse_args(argv)

    frame = command_frame(ns)
    wav = to_wav_bytes(render(frame, repeat=max(1, ns.repeat)))
    print("frame", frame.hex(), "nibbles", "".join("%X" % n for n in to_nibbles(frame)), file=sys.stderr)

    if ns.wav:
        with open(ns.wav, "wb") as f:
            f.write(wav)
        return 0
    if sys.platform == "win32":
        import winsound
        winsound.PlaySound(wav, winsound.SND_MEMORY)
        return 0
    out = "ks_cmd.wav"
    with open(out, "wb") as f:
        f.write(wav)
    print("not on Windows; wrote", out, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
