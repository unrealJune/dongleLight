"""Duplex audio control plane. WinMM is imported only on Windows.

Uplink: the existing four-channel tone codec. Downlink: 600-baud BFSK,
1000/1800 Hz, 8N1 bytes, 16 pilot bytes, D3 91, length (LE16), followed by
RS(255,223) blocks containing the payload and CRC32.
The dongle is the shared authority; playback alone is never called delivered.
"""
import array
import binascii
import json
import math
import queue
import socket
import struct
import sys
import threading
import time
import ks

if __name__ == "__main__" and "--devices" in sys.argv:
    from ks_winmm import devices
    print(json.dumps(devices()))
    sys.exit(0)

from reedsolo import RSCodec, ReedSolomonError

STATES = tuple(ks.SIGNALS)
PROVIDERS = ("copilot", "claude")
OP_STYLED, OP_RULE, OP_HELLO, OP_CLAIM = 7, 8, 9, 10
PAIRING_STATES = ("idle", "searching", "paired", "not-found", "full", "forgotten")
MAX_PAYLOAD = 512


def clean_name(name):
    return "".join(c for c in name if 32 <= ord(c) <= 126)[:8] or "devbox"


def styled_args(state, name, color, brightness, lamp):
    if state not in STATES or not 0 <= brightness <= 100 or not 0 <= lamp <= 4:
        raise ValueError("invalid signal rule")
    rgb = ks.parse_hex_color(color)
    name = clean_name(name)
    return [ks.source_id(name), lamp << 4 | ks.SIGNALS[state], *rgb, brightness, *name.encode()]


def parse_snapshot(payload):
    """Strictly decode the complete, CRC-checked snapshot, including shared rules."""
    pos = 0

    def take(n):
        nonlocal pos
        if pos + n > len(payload):
            raise ValueError("truncated snapshot")
        result = payload[pos:pos + n]
        pos += n
        return result

    def byte():
        return take(1)[0]

    def name(n):
        return take(n).split(b"\0", 1)[0].decode("ascii")

    if byte() != 2:
        raise ValueError("unsupported status version")
    link = byte()
    gen, applied = struct.unpack(">HH", take(4))
    result = byte()
    sources = []
    count = byte()
    if count > 8:
        raise ValueError("too many sources")
    for _ in range(count):
        sid, state, acked, lamp, length = take(5)
        if state >= 5 or lamp > 4 or length > 8:
            raise ValueError("invalid source")
        sources.append(dict(id=sid, state=STATES[state], acked=bool(acked),
                            lamp=lamp, name=name(length)))
    lamps = []
    count = byte()
    if count > 4:
        raise ValueError("too many lamps")
    for i in range(count):
        flags, length = take(2)
        if length > 19:
            raise ValueError("invalid lamp name")
        lamps.append(dict(number=i + 1, name=name(length), connected=bool(flags & 2),
                          readback=bool(flags & 4), verified=bool(flags & 8)))
    marker, version = take(2)
    if marker != 0xB1 or version not in (1, 2, 3):
        raise ValueError("duplex firmware required")
    flags = byte() if version == 3 else (1 | (2 if version == 2 else 0))
    if flags & ~3:
        raise ValueError("invalid snapshot flags")
    uptime, = struct.unpack("<I", take(4))
    for i in range(4):
        r, g, b, kind = take(4)
        if i < len(lamps):
            lamps[i].update(color=f"#{r:02X}{g:02X}{b:02X}", scene=kind)
    channels = []
    for ch in range(4):
        used, owner = take(2)
        channels.append(dict(channel=ch, used=bool(used), owner=owner, name=name(8)))
    rules = None
    if flags & 1:
        rules = []
        for ch in range(4):
            for provider in PROVIDERS:
                for state in STATES:
                    r, g, b, brightness, lamp = take(5)
                    if brightness > 100 or lamp > 4:
                        raise ValueError("invalid rule")
                    rules.append(dict(channel=ch, provider=provider, state=state,
                                      color=f"#{r:02X}{g:02X}{b:02X}",
                                      brightness=brightness, lamp=lamp))
    pairing = None
    if flags & 2:
        state, slot = take(2)
        since, = struct.unpack("<I", take(4))
        if state >= len(PAIRING_STATES) or slot >= 4:
            raise ValueError("invalid pairing state")
        pairing = dict(state=PAIRING_STATES[state], lamp=slot + 1, since=since)
    if pos != len(payload):
        raise ValueError("unexpected snapshot tail")
    return dict(link=link, generation=gen, applied=applied, result=result,
                uptime=uptime, sources=sources, lamps=lamps, channels=channels, rules=rules,
                pairing=pairing, snapshot_version=version)


BIT_SAMPLES = 80
BITS_PER_BYTE = 10
_RS = RSCodec(32)

_BASIS = [(tuple(math.cos(2 * math.pi * f * i / 48000) for i in range(BIT_SAMPLES)),
           tuple(math.sin(2 * math.pi * f * i / 48000) for i in range(BIT_SAMPLES)))
          for f in (1000, 1800)]


def decode_burst(samples):
    """Pilot acquisition plus per-UART-byte timing recovery (codec clock drift)."""
    cache = {}

    def bit(at):
        if at in cache:
            return cache[at]
        block = samples[at:at + BIT_SAMPLES]
        if len(block) != BIT_SAMPLES:
            return 0, 0.0
        energies = []
        for co, si in _BASIS:
            re = sum(x * y for x, y in zip(block, co))
            im = sum(x * y for x, y in zip(block, si))
            energies.append(re * re + im * im)
        a, b = energies
        value = int(b > a)
        confidence = abs(b - a) / (a + b + 1)
        cache[at] = value, confidence
        return value, confidence

    def uart(at):
        bits = [bit(at + i * BIT_SAMPLES) for i in range(BITS_PER_BYTE)]
        value = sum(bits[i + 1][0] << i for i in range(8))
        framing_penalty = int(bits[0][0] != 0) + int(bits[-1][0] != 1)
        return value, sum(v[1] for v in bits) - framing_penalty * 2

    sync = "".join("0" + "".join(str((byte >> i) & 1) for i in range(8)) + "1" for byte in (0xD3, 0x91))
    sync_word = int(sync, 2)
    candidates = []
    for phase in range(0, BIT_SAMPLES, 4):
        # Keep the full pilot/sync after a partially silent 100 ms capture buffer.
        acquisition = 32 * BITS_PER_BYTE * BIT_SAMPLES
        bits = "".join(str(bit(at)[0]) for at in range(phase, min(len(samples) - BIT_SAMPLES, acquisition), BIT_SAMPLES))
        for start in range(len(bits) - len(sync) + 1):
            errors = (int(bits[start:start + len(sync)], 2) ^ sync_word).bit_count()
            if errors <= 2:
                candidates.append((errors, phase + start * BIT_SAMPLES + 2 * BITS_PER_BYTE * BIT_SAMPLES))
    # Prefer exact sync, but let RS and CRC32 validate a damaged sync candidate.
    candidates.sort()
    for start, adaptive in [(start, adaptive) for adaptive in (False, True) for _, start in candidates]:
        data = bytearray()
        at = start
        expected = None
        while at + BITS_PER_BYTE * BIT_SAMPLES + 20 < len(samples) and len(data) < MAX_PAYLOAD + 102:
            offsets = range(-20, 21, 4) if adaptive else (0,)
            options = [(uart(at + delta), at + delta) for delta in offsets if at + delta >= 0]
            ((value, _), best) = max(options, key=lambda entry: entry[0][1])
            # A weak UART byte is recoverable by RS; do not truncate its block.
            data.append(value)
            at = best + BITS_PER_BYTE * BIT_SAMPLES
            if len(data) == 2:
                length = int.from_bytes(data, "little")
                if not 1 <= length <= MAX_PAYLOAD:
                    break
                raw_length = length + 4
                expected = 2 + raw_length + ((raw_length + 222) // 223) * 32
            if expected and len(data) == expected:
                try:
                    decoded = _RS.decode(data[2:])[0]
                    if binascii.crc32(decoded[:-4]) == int.from_bytes(decoded[-4:], "little"):
                        return bytes(decoded[:-4])
                except ReedSolomonError:
                    pass
                break
    return None


class Receiver:
    def __init__(self, callback, asynchronous=False):
        self.callback = callback
        self.samples = array.array("h")
        self.quiet = 0
        self.peak = 0
        self.pending = queue.Queue(maxsize=2) if asynchronous else None
        if self.pending is not None:
            threading.Thread(target=self._run, daemon=True).start()

    def _decode(self, samples):
        payload = decode_burst(samples)
        if payload:
            try:
                self.callback(parse_snapshot(payload))
            except (ValueError, UnicodeError, IndexError):
                pass

    def _run(self):
        while True:
            self._decode(self.pending.get())

    def feed(self, pcm):
        block = array.array("h", pcm)
        if sys.byteorder != "little":
            block.byteswap()
        level = sum(abs(x) for x in block) / max(1, len(block))
        self.peak = max(level, self.peak * 0.98)
        if level > max(8, self.peak * 0.06):
            self.quiet = 0
            self.samples.extend(block)
        elif self.samples:
            self.quiet += len(block)
            self.samples.extend(block)
            if self.quiet >= 4800:
                samples = self.samples
                self.samples = array.array("h")
                if self.pending is None:
                    self._decode(samples)
                else:
                    try:
                        self.pending.put_nowait(samples)
                    except queue.Full:
                        # Complete snapshots repeat; never stall microphone buffer recycling.
                        pass
        if len(self.samples) > 600000:
            self.samples = array.array("h")
            self.quiet = 0


class Worker:
    def __init__(self, config, emit, audio_factory=None):
        from ks_winmm import Audio
        self.config, self.emit = config, emit
        self.name = clean_name(config.get("name") or socket.gethostname())
        self.channel = int(config.get("channel", 0))
        if not 0 <= self.channel <= 3:
            raise ValueError("channel must be 0..3")
        self.latest = None
        self.last_received = 0
        self.last_full_received = 0
        self.last_hello = 0
        self.lock = threading.Lock()
        self.changed = threading.Event()
        self.receiver = Receiver(self.on_snapshot, asynchronous=True)
        self.audio = (audio_factory or Audio)(config.get("output", -1), config.get("input", -1),
                                               self.receiver.feed, self.on_error)

    def on_error(self, error):
        self.changed.set()
        self.emit(dict(kind="error", message=str(error)))

    def on_snapshot(self, snapshot):
        now = time.monotonic()
        with self.lock:
            full = snapshot["rules"] is not None
            if full:
                self.last_full_received = now
            elif self.latest and self.latest["rules"] is not None:
                snapshot["rules"] = self.latest["rules"]
            self.latest = snapshot
            self.last_received = now
        self.changed.set()
        if full or snapshot["rules"] is not None:
            self.emit(dict(kind="snapshot", **snapshot))

    def send(self, op, args, repeat=2):
        samples = ks.render(ks.build_frame(op, args), repeat=repeat, channel=self.channel)
        pcm = array.array("h", (max(-32767, min(32767, round(x * 32767))) for x in samples))
        if sys.byteorder != "little":
            pcm.byteswap()
        self.audio.play(pcm.tobytes())

    def hello(self):
        self.send(OP_HELLO, [ks.source_id(self.name), self.channel, *self.name.encode()])
        self.last_hello = time.monotonic()

    @staticmethod
    def _generation_after(current, previous):
        difference = (current - previous) & 0xFFFF
        return 0 < difference < 0x8000

    def _wait_generation(self, previous, timeout=None):
        if timeout is None:
            version = self.latest["snapshot_version"] if self.latest else 1
            timeout = 5 if version >= 3 else 12
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._check_audio()
            with self.lock:
                current = self.latest
                fresh = time.monotonic() - self.last_received < 15
            if current and fresh and self._generation_after(current["generation"], previous):
                return current
            self.changed.wait(min(0.5, max(0, deadline - time.monotonic())))
            self.changed.clear()
        return None

    def _send_confirmed(self, op, args):
        if self.last_hello and time.monotonic() - self.last_hello > 45:
            previous = self.latest["generation"]
            self.hello()
            if self._wait_generation(previous, timeout=10) is None:
                raise TimeoutError("Channel renewal was not acknowledged.")
        for _ in range(2):
            self._check_audio()
            with self.lock:
                previous = self.latest["generation"]
            self.send(op, args)
            snapshot = self._wait_generation(previous)
            if snapshot is not None:
                return snapshot
        raise TimeoutError("Command was not acknowledged; check the audio link and retry.")

    def _check_audio(self):
        failure = getattr(getattr(self, "audio", None), "failed", None)
        if failure is not None:
            raise failure

    def _channel_ready(self):
        self._check_audio()
        with self.lock:
            snapshot = self.latest
            fresh = time.monotonic() - self.last_received < 15
        if snapshot and fresh:
            owner = snapshot["channels"][self.channel]
            if owner["used"]:
                if owner["name"] != self.name or owner["owner"] != ks.source_id(self.name):
                    raise ValueError(f"Channel {self.channel} belongs to {owner['name']}; choose a free channel.")
                if snapshot["rules"] is not None:
                    return snapshot
        return None

    def ready(self, timeout=40):
        snapshot = self._channel_ready()
        if snapshot and getattr(self, "last_hello", 0) > 0:
            return snapshot
        if time.monotonic() - self.last_hello > 10 or not self.last_hello:
            self.hello()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            snapshot = self._channel_ready()
            if snapshot:
                return snapshot
            self.changed.wait(1)
            self.changed.clear()
            if time.monotonic() - self.last_hello > 10:
                self.hello()
        raise TimeoutError("No return audio or channel acknowledgement. Check RDP microphone redirection and input device.")

    def execute(self, request):
        action = request["action"]
        snapshot = self.ready()
        if action == "claim":
            pairing = snapshot.get("pairing")
            if pairing is None:
                raise ValueError("Update the dongle firmware to add lamps from the app. BOOT pairing still works.")
            if len(snapshot["lamps"]) >= 4:
                raise ValueError("All four lamp slots are already paired.")
            if pairing["state"] == "searching":
                raise ValueError("A lamp search is already running. Wait for its result.")
            previous = pairing["since"]
            count = len(snapshot["lamps"])
            self.send(OP_CLAIM, [])
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                self._check_audio()
                with self.lock:
                    current = self.latest
                    fresh = time.monotonic() - self.last_received < 15
                status = current.get("pairing")
                if fresh and status and status["since"] != previous:
                    if status["state"] == "paired" and len(current["lamps"]) > count:
                        lamp = next((l for l in current["lamps"] if l["number"] == status["lamp"]), None)
                        if lamp:
                            return dict(kind="result", id=request.get("id"), action=action, ok=True,
                                        message=f"Lamp {lamp['number']} paired: {lamp['name']}.")
                    if status["state"] == "not-found":
                        raise ValueError("No unpaired lamp found. Power it on nearby and close its phone app, then try Add lamp again.")
                    if status["state"] == "full":
                        raise ValueError("All four lamp slots are already paired.")
                    if status["state"] == "forgotten":
                        raise ValueError("Pairing was interrupted because the dongle's lamp memory was cleared.")
                self.changed.wait(0.5)
                self.changed.clear()
            raise TimeoutError("Pairing was not confirmed. Check the shared lamp list and return audio before trying again.")
        elif action == "rules":
            rules_changed = False
            for rule in request["rules"]:
                provider, state = rule["provider"], rule["state"]
                if provider not in PROVIDERS or state not in STATES:
                    raise ValueError("unknown provider or event")
                args = [self.channel, PROVIDERS.index(provider), STATES.index(state),
                        *ks.parse_hex_color(rule["color"]), int(rule["brightness"]), int(rule["lamp"])]
                if not 0 <= args[6] <= 100 or not 0 <= args[7] <= 4:
                    raise ValueError("invalid brightness or lamp")
                if any(all(r.get(k) == rule.get(k) for k in ("provider", "state", "color", "brightness", "lamp"))
                       for r in snapshot["rules"] if r["channel"] == self.channel):
                    continue
                rules_changed = True
                for attempt in range(3):
                    with self.lock:
                        previous = self.latest["generation"]
                    self.send(OP_RULE, args)
                    if self._wait_generation(previous) is not None:
                        break
                    if attempt == 2:
                        raise TimeoutError("Rule was not acknowledged by the dongle; retry when return audio is live.")
            if rules_changed:
                previous_full = self.last_full_received
                self.hello()
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    self._check_audio()
                    with self.lock:
                        current = self.latest
                        full = self.last_full_received > previous_full
                    if full and current and all(any(
                            r["channel"] == self.channel and all(
                                r.get(k) == rule.get(k)
                                for k in ("provider", "state", "color", "brightness", "lamp"))
                            for r in current["rules"]) for rule in request["rules"]):
                        break
                    self.changed.wait(0.5)
                    self.changed.clear()
                else:
                    raise TimeoutError("Updated rules were not confirmed in a full shared-state snapshot.")
        elif action == "signal":
            provider, state = request["provider"], request["state"]
            if provider not in PROVIDERS or state not in STATES:
                raise ValueError("unknown provider or event")
            rule = next(r for r in snapshot["rules"] if r["channel"] == self.channel
                        and r["provider"] == provider and r["state"] == state)
            # Two source slots per devbox; provider + channel makes labels unambiguous.
            name = f"{self.name[:5]}{self.channel}{'C' if provider == 'copilot' else 'A'}"
            args = styled_args(state, name, rule["color"], rule["brightness"], rule["lamp"])
            args[0] = 0xE0 + self.channel * 2 + PROVIDERS.index(provider)
            self._send_confirmed(OP_STYLED, args)
        elif action == "preview":
            if request["provider"] not in PROVIDERS:
                raise ValueError("unknown provider")
            name = f"{self.name[:5]}{self.channel}{'C' if request['provider'] == 'copilot' else 'A'}"
            args = styled_args(request["state"], name, request["color"],
                               int(request["brightness"]), int(request["lamp"]))
            args[0] = 0xE0 + self.channel * 2 + PROVIDERS.index(request["provider"])
            self._send_confirmed(OP_STYLED, args)
        elif action == "ack":
            self._send_confirmed(ks.OP_ACK, ks.lamp_mask(int(request.get("lamp", 0))))
        elif action == "rgb":
            brightness, lamp = int(request["brightness"]), int(request.get("lamp", 0))
            if not 0 <= brightness <= 100 or not 0 <= lamp <= 4:
                raise ValueError("invalid brightness or lamp")
            self._send_confirmed(ks.OP_RGB, [*ks.parse_hex_color(request["color"]), brightness, *ks.lamp_mask(lamp)])
        elif action == "off":
            lamp = int(request.get("lamp", 0))
            if not 0 <= lamp <= 4:
                raise ValueError("invalid lamp")
            self._send_confirmed(ks.OP_POWER, [0, *ks.lamp_mask(lamp)])
        elif action != "connect":
            raise ValueError("unknown action")
        return dict(kind="result", id=request.get("id"), action=action, ok=True,
                    message="Rules confirmed by dongle" if action == "rules" else
                    "Duplex channel confirmed" if action == "connect" else
                    "Command confirmed by dongle")

    def close(self):
        self.audio.close()


def main():
    from ks_winmm import devices
    if "--devices" in sys.argv:
        print(json.dumps(devices()))
        return
    output_lock = threading.Lock()

    def emit(value):
        with output_lock:
            print(json.dumps(value), flush=True)

    config = json.loads(sys.stdin.readline())
    work = queue.Queue(maxsize=64)
    worker = Worker(config, emit)

    def read():
        for line in sys.stdin:
            try:
                work.put_nowait(json.loads(line))
            except (ValueError, queue.Full) as error:
                emit(dict(kind="error", message=str(error)))
        work.put(None)

    threading.Thread(target=read, daemon=True).start()
    pending = []
    try:
        while True:
            if getattr(worker.audio, "failed", None) is not None:
                break
            if not pending:
                try:
                    item = work.get(timeout=1)
                except queue.Empty:
                    if getattr(worker.audio, "failed", None) is not None:
                        break
                    if time.monotonic() - worker.last_hello >= 45:
                        try:
                            previous = worker.latest["generation"] if worker.latest else None
                            worker.hello()
                            if previous is not None and worker._wait_generation(previous, timeout=10) is None:
                                raise TimeoutError("Channel renewal was not acknowledged.")
                        except Exception as error:
                            worker.on_error(error)
                            if isinstance(error, OSError):
                                break
                    continue
                pending.append(item)
            while True:
                try:
                    pending.append(work.get_nowait())
                except queue.Empty:
                    break
            request = pending.pop(0)
            if request is None:
                break
            if request.get("action") in ("rgb", "off"):
                lamp = int(request.get("lamp", 0))
                if any(r is not None and r.get("action") in ("rgb", "off") and
                       (int(r.get("lamp", 0)) in (0, lamp)) for r in pending):
                    emit(dict(kind="result", id=request.get("id"), action=request.get("action"),
                              ok=True, message="Superseded by newer light command"))
                    continue
            try:
                emit(worker.execute(request))
            except Exception as error:
                emit(dict(kind="result", id=request.get("id"), action=request.get("action"),
                          ok=False, message=str(error)))
                if isinstance(error, OSError) or getattr(worker.audio, "failed", None) is not None:
                    break
    finally:
        worker.close()


if __name__ == "__main__":
    main()
