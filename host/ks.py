#!/usr/bin/env python3
"""ks.py: talk to the KS light bridge from a devbox. Stdlib only; copy anywhere.

Two transports reach the dongle on your laptop through RDP:
  card   redirected smart card (two-way: you get "delivered" back)
  audio  tones played through redirected audio (one-way, always available)
--via auto (default) uses the card when a bridge reader answers, else audio.

Lamp:     ks.py rgb ff8800 [--brightness 40] | on | off | white 200
          ks.py effect purple-breathing [--speed 40]
Agents:   ks.py signal needs-you|working|done|error|idle [--source NAME]
          ks.py ack [--lamp N]      # same as pressing the dongle's button
          ks.py status              # card only
Lamps:    manual commands take --lamp N (default: every lamp). Each devbox is
          auto-assigned its own lamp on first signal; KS_LAMP=N pins it.
Setup:    ks.py install-hooks [--dry-run]   # wire Claude Code to the light

Env: KS_SOURCE (name shown on the dongle, default: hostname),
     KS_LAMP (1-4: pin this devbox's signals to a lamp, default: auto),
     KS_CHANNEL (audio channel 0-3, default: from the source name),
     KS_VIA (auto|card|audio).
"""
import argparse
import ctypes
import io
import json
import math
import os
import random
import socket
import struct
import sys
import time
import wave

# ----------------------------------------------------------------- frames

VERSION = 0x5
OP_POWER, OP_RGB, OP_EFFECT, OP_WHITE, OP_SIGNAL, OP_ACK = 1, 2, 3, 4, 5, 6
SIGNALS = {"idle": 0, "working": 1, "done": 2, "error": 3, "needs-you": 4}
SIGNAL_NAMES = {v: k for k, v in SIGNALS.items()}
NAME_MAX = 8

# KS03~ native effects (from H4ch1Net/ks-led-controller ks_light/protocol.py).
EFFECTS = {
    "seven-color-fade": 0x82, "rgb-fade": 0x83, "red-breathing": 0x84,
    "green-breathing": 0x85, "blue-breathing": 0x86, "yellow-breathing": 0x87,
    "cyan-breathing": 0x88, "purple-breathing": 0x89, "white-breathing": 0x8A,
}


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_body(op, args):
    for a in args:
        if not 0 <= a <= 255:
            raise ValueError("argument out of byte range: %r" % a)
    return bytes([(VERSION << 4) | op, *args])


def build_frame(op, args):
    """Audio frame: body + CRC-8."""
    body = build_body(op, args)
    return body + bytes([crc8(body)])


def source_name():
    name = os.environ.get("KS_SOURCE") or socket.gethostname().split(".")[0]
    name = "".join(ch for ch in name if 0x20 <= ord(ch) <= 0x7E)
    return name[:NAME_MAX] or "devbox"


def source_id(name):
    return crc8(name.encode())


MAX_LAMPS = 4


def signal_args(state, name, lamp=0):
    """lamp 0 = let the dongle assign one; 1..4 pins this devbox to that lamp."""
    if not 0 <= lamp <= MAX_LAMPS:
        raise ValueError("lamp must be 1..%d" % MAX_LAMPS)
    return [source_id(name), (lamp << 4) | SIGNALS[state], *name.encode()[:NAME_MAX]]


def lamp_mask(lamp):
    """Trailing mask byte for manual ops: [] = every lamp."""
    return [] if not lamp else [1 << (lamp - 1)]


def env_lamp():
    v = os.environ.get("KS_LAMP", "").strip()
    return int(v) if v else 0

# ----------------------------------------------------------------- audio

RATE = 48000
TONE_MS, GAP_MS = 50, 30
LEAD_MS, LEAD_HZ = 250, 440.0   # non-grid lead-in: wakes the audio path, ignored by the decoder
TAIL_MS = 250                   # silence that closes a frame (decoder needs >= 150 ms)
TONE_AMP, RAMP_MS = 0.3, 3

# Interleaved tone grid shared with the C receiver: channel c owns slots
# c, c+4, ..., c+28 (rows are the first four, cols the last four).
GRID_F0, GRID_STEP, MAX_CHANNELS = 600.0, 85.0, 4
# nibble -> (row, col); '*' = 0xE, '#' = 0xF in DTMF terms
KEYPOS = {
    0x1: (0, 0), 0x2: (0, 1), 0x3: (0, 2), 0xA: (0, 3),
    0x4: (1, 0), 0x5: (1, 1), 0x6: (1, 2), 0xB: (1, 3),
    0x7: (2, 0), 0x8: (2, 1), 0x9: (2, 2), 0xC: (2, 3),
    0xE: (3, 0), 0x0: (3, 1), 0xF: (3, 2), 0xD: (3, 3),
}


def tone_hz(channel, i):
    return GRID_F0 + GRID_STEP * (channel + MAX_CHANNELS * i)


def to_nibbles(frame):
    out = []
    for b in frame:
        out += [b >> 4, b & 0x0F]
    return out


def _samples(ms):
    return int(RATE * ms / 1000)


def _tone(freqs, ms, amp):
    n, ramp = _samples(ms), _samples(RAMP_MS)
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


def render(frame, repeat=2, channel=0, rng=None):
    """Mono samples: lead-in, then the frame `repeat` times with random gaps
    (so two devboxes that collide once are unlikely to collide again)."""
    rng = rng or random.Random()
    audio = _tone((LEAD_HZ,), LEAD_MS, 0.15) + [0.0] * _samples(60)
    for k in range(repeat):
        for nib in to_nibbles(frame):
            r, c = KEYPOS[nib]
            audio += _tone((tone_hz(channel, r), tone_hz(channel, 4 + c)), TONE_MS, TONE_AMP)
            audio += [0.0] * _samples(GAP_MS)
        gap = rng.randint(300, 900) if k < repeat - 1 else TAIL_MS
        audio += [0.0] * _samples(gap)
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


def default_channel():
    if os.environ.get("KS_CHANNEL"):
        return int(os.environ["KS_CHANNEL"]) % MAX_CHANNELS
    return source_id(source_name()) % MAX_CHANNELS


def send_audio(op, args, channel, repeat=2, wav_path=None):
    wav = to_wav_bytes(render(build_frame(op, args), repeat=max(1, repeat), channel=channel))
    if wav_path:
        with open(wav_path, "wb") as f:
            f.write(wav)
        return "wrote %s" % wav_path
    if sys.platform == "win32":
        import winsound
        winsound.PlaySound(wav, winsound.SND_MEMORY)
        return "played on channel %d" % channel
    out = "ks_cmd.wav"
    with open(out, "wb") as f:
        f.write(wav)
    return "not on Windows; wrote %s" % out

# ----------------------------------------------------------------- smart card

AID = bytes([0xF0]) + b"KSLIGHT"
INS_SUBMIT, INS_STATUS = 0x10, 0x20
RESULTS = {0: "ok", 1: "no lamp connected", 2: "lamp write failed", 3: "not supported by this lamp", 0xFF: "pending"}
LINKS = {0: "booting", 1: "searching for lamp", 2: "connected"}


class CardError(Exception):
    pass


def apdu_select():
    return bytes([0x00, 0xA4, 0x04, 0x00, len(AID)]) + AID


def apdu_submit(body):
    return bytes([0x80, INS_SUBMIT, 0x00, 0x00, len(body)]) + body


def apdu_status():
    return bytes([0x80, INS_STATUS, 0x00, 0x00, 0x00])


def parse_status(data):
    """Status record v2 (see ksc_card.h)."""
    if len(data) < 8 or data[0] != 2:
        raise CardError("unexpected status record %s (update ks.py or the firmware)" % data.hex())
    st = {"link": LINKS.get(data[1], "?"), "desired_gen": data[2] << 8 | data[3],
          "applied_gen": data[4] << 8 | data[5], "last_result": RESULTS.get(data[6], "?"),
          "sources": [], "lamps": []}
    n, i = data[7], 8
    for _ in range(n):
        sid, state, acked, lamp, nl = data[i:i + 5]
        name = data[i + 5:i + 5 + nl].decode("ascii", "replace")
        st["sources"].append({"id": sid, "state": SIGNAL_NAMES.get(state, "?"), "acked": bool(acked),
                              "lamp": lamp or None, "name": name})
        i += 5 + nl
    for _ in range(data[i]):
        flags, nl = data[i + 1], data[i + 2]
        st["lamps"].append({"claimed": bool(flags & 1), "connected": bool(flags & 2),
                            "name": data[i + 3:i + 3 + nl].decode("ascii", "replace")})
        i += 2 + nl
    return st


class PCSC:
    """Just enough WinSCard / pcsc-lite through ctypes."""
    SCOPE_USER, SHARE_SHARED, PROTO_T1, LEAVE_CARD = 0, 2, 2, 0

    def __init__(self):
        if sys.platform == "win32":
            self.lib = ctypes.WinDLL("winscard")
            self.DWORD = ctypes.c_uint32
            self.wide = True
        elif sys.platform == "darwin":
            self.lib = ctypes.CDLL("/System/Library/Frameworks/PCSC.framework/PCSC")
            self.DWORD = ctypes.c_uint32
            self.wide = False
        else:
            self.lib = ctypes.CDLL("libpcsclite.so.1")
            self.DWORD = ctypes.c_ulong
            self.wide = False
        self.HANDLE = ctypes.c_size_t if sys.platform == "win32" else ctypes.c_long
        self.ctx = self.HANDLE()
        self._check(self.lib.SCardEstablishContext(self.DWORD(self.SCOPE_USER), None, None, ctypes.byref(self.ctx)),
                    "SCardEstablishContext")

        class IoRequest(ctypes.Structure):
            _fields_ = [("dwProtocol", self.DWORD), ("cbPciLength", self.DWORD)]
        self.pci_t1 = IoRequest(self.PROTO_T1, ctypes.sizeof(IoRequest))

    @staticmethod
    def _check(rc, what):
        rc &= 0xFFFFFFFF
        if rc:
            known = {0x8010002E: "no smart card readers (is smart card redirection on?)",
                     0x8010000C: "no card in reader", 0x80100069: "card removed",
                     0x8010000B: "sharing violation (another session holds the card)",
                     0x8010001D: "smart card service not running"}
            raise CardError("%s failed: %s (0x%08X)" % (what, known.get(rc, "error"), rc))

    def readers(self):
        fn = self.lib.SCardListReadersW if self.wide else self.lib.SCardListReaders
        n = self.DWORD(0)
        rc = fn(self.ctx, None, None, ctypes.byref(n)) & 0xFFFFFFFF
        if rc == 0x8010002E:
            return []
        self._check(rc, "SCardListReaders")
        buf = ctypes.create_unicode_buffer(n.value) if self.wide else ctypes.create_string_buffer(n.value)
        self._check(fn(self.ctx, None, buf, ctypes.byref(n)), "SCardListReaders")
        raw = buf[:n.value]
        if not self.wide:
            raw = raw.decode("utf-8", "replace")
        return [r for r in raw.split("\0") if r]

    def connect(self, reader):
        fn = self.lib.SCardConnectW if self.wide else self.lib.SCardConnect
        card, proto = self.HANDLE(), self.DWORD()
        name = reader if self.wide else reader.encode()
        self._check(fn(self.ctx, name, self.DWORD(self.SHARE_SHARED), self.DWORD(self.PROTO_T1),
                       ctypes.byref(card), ctypes.byref(proto)), "SCardConnect")
        return card

    def begin(self, card):
        self._check(self.lib.SCardBeginTransaction(card), "SCardBeginTransaction")

    def end(self, card):
        self.lib.SCardEndTransaction(card, self.DWORD(self.LEAVE_CARD))

    def transmit(self, card, apdu):
        out = ctypes.create_string_buffer(258)
        n = self.DWORD(len(out))
        self._check(self.lib.SCardTransmit(card, ctypes.byref(self.pci_t1), apdu, self.DWORD(len(apdu)), None,
                                           out, ctypes.byref(n)), "SCardTransmit")
        resp = out.raw[:n.value]
        if len(resp) < 2:
            raise CardError("short response")
        return resp[:-2], resp[-2] << 8 | resp[-1]

    def disconnect(self, card):
        self.lib.SCardDisconnect(card, self.DWORD(self.LEAVE_CARD))

    def close(self):
        self.lib.SCardReleaseContext(self.ctx)


class Bridge:
    """A bridge card found among the visible readers."""

    def __init__(self):
        self.pcsc = PCSC()
        self.card = None
        for reader in self.pcsc.readers():
            try:
                card = self.pcsc.connect(reader)
            except CardError:
                continue
            try:
                _, sw = self.pcsc.transmit(card, apdu_select())
            except CardError:
                sw = 0
            if sw == 0x9000:
                self.card, self.reader = card, reader
                break
            self.pcsc.disconnect(card)
        if self.card is None:
            self.pcsc.close()
            raise CardError("no KS bridge reader found")

    def exchange(self, apdu):
        # One transaction per command: other devboxes' sessions can't interleave
        # between our SELECT and our command.
        self.pcsc.begin(self.card)
        try:
            data, sw = self.pcsc.transmit(self.card, apdu_select())
            if sw != 0x9000:
                raise CardError("SELECT failed: %04X" % sw)
            data, sw = self.pcsc.transmit(self.card, apdu)
        finally:
            self.pcsc.end(self.card)
        if sw != 0x9000:
            raise CardError({0x6A80: "bridge rejected the command", 0x6F00: "bridge busy",
                             0x6985: "applet not selected"}.get(sw, "status word %04X" % sw))
        return data

    def submit(self, op, args):
        data = self.exchange(apdu_submit(build_body(op, args)))
        return data[0] << 8 | data[1]

    def status(self):
        return parse_status(self.exchange(apdu_status()))

    def wait_applied(self, gen, timeout=10.0):
        end = time.monotonic() + timeout
        while True:
            st = self.status()
            done = ((st["applied_gen"] - gen) & 0xFFFF) < 0x8000
            if done or time.monotonic() > end:
                return st, done
            time.sleep(0.15)

    def close(self):
        self.pcsc.disconnect(self.card)
        self.pcsc.close()

# ----------------------------------------------------------------- hooks

HOOK_EVENTS = [  # (event, matcher, signal)
    ("Notification", "permission_prompt|idle_prompt", "needs-you"),
    ("UserPromptSubmit", None, "working"),
    ("Stop", None, "done"),
    ("StopFailure", None, "error"),
    ("SessionEnd", None, "idle"),
]


def install_hooks(dry_run=False, settings_path=None):
    path = settings_path or os.path.join(os.path.expanduser("~"), ".claude", "settings.json")
    try:
        with open(path) as f:
            settings = json.load(f)
    except FileNotFoundError:
        settings = {}
    script = os.path.abspath(__file__)
    py = sys.executable or "python"
    hooks = settings.setdefault("hooks", {})
    added = 0
    for event, matcher, sig in HOOK_EVENTS:
        groups = hooks.setdefault(event, [])
        if any("ks.py" in h.get("command", "") and " signal " in h.get("command", "")
               for g in groups for h in g.get("hooks", [])):
            continue
        group = {"hooks": [{"type": "command", "async": True, "timeout": 30,
                            "command": '"%s" "%s" signal %s --quiet' % (py, script, sig)}]}
        if matcher:
            group = {"matcher": matcher, **group}
        groups.append(group)
        added += 1
    text = json.dumps(settings, indent=2)
    if dry_run:
        return text
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text + "\n")
    return "added %d hook(s) to %s" % (added, path)

# ----------------------------------------------------------------- CLI


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


def command(ns):
    mask = lamp_mask(getattr(ns, "lamp", None))
    if ns.cmd in ("on", "off"):
        return OP_POWER, [1 if ns.cmd == "on" else 0, *mask]
    if ns.cmd == "rgb":
        return OP_RGB, [*ns.color, ns.brightness, *mask]
    if ns.cmd == "effect":
        return OP_EFFECT, [EFFECTS[ns.name], ns.speed, max(1, ns.brightness), *mask]
    if ns.cmd == "white":
        return OP_WHITE, [ns.level, *mask]
    if ns.cmd == "signal":
        lamp = ns.lamp if ns.lamp is not None else env_lamp()
        return OP_SIGNAL, signal_args(ns.state, ns.source or source_name(), lamp)
    if ns.cmd == "ack":
        return OP_ACK, mask
    raise SystemExit("unknown command")


def describe(st):
    lines = []
    for n, lamp in enumerate(st["lamps"], 1):
        state = "connected" if lamp["connected"] else "claimed, searching" if lamp["claimed"] else "unclaimed"
        lines.append("lamp %d: %s (%s)" % (n, lamp["name"] or "-", state))
    if not st["lamps"]:
        lines.append("no lamps claimed yet: long-press BOOT near a lamp")
    lines.append("last write: %s" % st["last_result"])
    for s in st["sources"]:
        where = "lamp %d" % s["lamp"] if s["lamp"] else "all lamps"
        lines.append("  %-8s %-9s on %s%s" % (s["name"], s["state"], where, " (acked)" if s["acked"] else ""))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--via", choices=("auto", "card", "audio"), default=os.environ.get("KS_VIA", "auto"))
    ap.add_argument("--channel", type=int, help="audio channel 0-3")
    ap.add_argument("--wav", help="audio: write a WAV instead of playing")
    ap.add_argument("--repeat", type=int, default=2, help="audio: copies per command")
    ap.add_argument("--no-wait", action="store_true", help="card: don't wait for the lamp to update")
    ap.add_argument("--quiet", "-q", action="store_true", help="no output, never fail (for hooks)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def lamp_opt(p, help_text="only this lamp (default: every lamp)"):
        p.add_argument("--lamp", type=int, choices=range(1, MAX_LAMPS + 1), help=help_text)
        return p

    lamp_opt(sub.add_parser("on"))
    lamp_opt(sub.add_parser("off"))
    p = lamp_opt(sub.add_parser("rgb"))
    p.add_argument("color", type=parse_hex_color)
    p.add_argument("--brightness", type=pct, default=100)
    p = lamp_opt(sub.add_parser("effect"))
    p.add_argument("name", choices=sorted(EFFECTS))
    p.add_argument("--speed", type=pct, default=50)
    p.add_argument("--brightness", type=pct, default=100)
    p = lamp_opt(sub.add_parser("white", help="KS03~ white channel, 0..255"))
    p.add_argument("level", type=int, choices=range(256), metavar="LEVEL")
    p = lamp_opt(sub.add_parser("signal", help="agent state for this devbox"),
                 "pin this devbox to a lamp (default: KS_LAMP or auto)")
    p.add_argument("state", choices=list(SIGNALS))
    p.add_argument("--source", help="name on the dongle screen (default: KS_SOURCE or hostname)")
    lamp_opt(sub.add_parser("ack", help="acknowledge attention signals"))
    sub.add_parser("status", help="show bridge state (card only)")
    p = sub.add_parser("install-hooks", help="add Claude Code hooks to ~/.claude/settings.json")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--settings", help="settings file path")
    ns = ap.parse_args(argv)

    def say(msg):
        if not ns.quiet:
            print(msg)

    try:
        if ns.cmd == "install-hooks":
            say(install_hooks(ns.dry_run, ns.settings))
            return 0
        if ns.cmd == "status":
            b = Bridge()
            try:
                say(describe(b.status()))
            finally:
                b.close()
            return 0
        op, args = command(ns)
        if ns.via in ("auto", "card") and not ns.wav:
            try:
                b = Bridge()
            except (CardError, OSError) as e:
                if ns.via == "card":
                    raise
                say("card: %s; falling back to audio" % e)
            else:
                try:
                    gen = b.submit(op, args)
                    if ns.no_wait:
                        say("queued (gen %d)" % gen)
                        return 0
                    st, done = b.wait_applied(gen)
                    say(("delivered: " if done else "timed out waiting: ") + st["last_result"])
                    return 0 if done and st["last_result"] == "ok" else 1
                finally:
                    b.close()
        channel = default_channel() if ns.channel is None else ns.channel % MAX_CHANNELS
        say(send_audio(op, args, channel, ns.repeat, ns.wav))
        return 0
    except (CardError, OSError) as e:
        if ns.quiet:
            return 0
        print("error:", e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
