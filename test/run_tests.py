#!/usr/bin/env python3
"""Build the portable C core and push ks_tx.py audio through it.

Scenarios approximate what RDP audio redirection might do: lossy codecs at low
bitrates, gain changes, noise, a notification chime landing mid-frame, and a
small clock/pitch error. Needs gcc, ffmpeg (with libopus/aac/mp3), numpy.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORE = os.path.join(ROOT, "firmware", "components", "ksaudio")
sys.path.insert(0, os.path.join(ROOT, "host"))
import ks_tx  # noqa: E402

BUILD = tempfile.mkdtemp(prefix="ksa_build_")
RATE = 48000


def build():
    srcs = [os.path.join(CORE, f) for f in ("ksa_dtmf_rx.c", "ksa_frame.c", "ksa_protocol.c")]
    inc = ["-I", os.path.join(CORE, "include")]
    flags = ["-O2", "-Wall", "-Wextra", "-Werror", "-std=c11"]
    dec = os.path.join(BUILD, "host_decode")
    proto = os.path.join(BUILD, "test_protocol")
    subprocess.run(["gcc", *flags, *inc, os.path.join(HERE, "host_decode.c"), *srcs, "-lm", "-o", dec], check=True)
    subprocess.run(["gcc", *flags, *inc, os.path.join(HERE, "test_protocol.c"), *srcs, "-lm", "-o", proto], check=True)
    return dec, proto


def stereo_s16(mono):
    a = np.clip(np.asarray(mono, dtype=np.float64), -1, 1)
    s = (a * 32767).round().astype("<i2")
    return np.repeat(s, 2).tobytes()


def ffmpeg_roundtrip(pcm, codec_args, ext, af=None):
    src = os.path.join(BUILD, "in.raw")
    enc = os.path.join(BUILD, "enc." + ext)
    with open(src, "wb") as f:
        f.write(pcm)
    raw_in = ["-f", "s16le", "-ar", str(RATE), "-ac", "2", "-i", src]
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *raw_in, *codec_args, enc], check=True)
    dec_args = ["ffmpeg", "-loglevel", "error", "-i", enc]
    if af:
        dec_args += ["-af", af]
    dec_args += ["-f", "s16le", "-ar", str(RATE), "-ac", "2", "-"]
    return subprocess.run(dec_args, check=True, capture_output=True).stdout


def decode(dec, pcm):
    p = subprocess.run([dec], input=pcm, capture_output=True, check=True)
    lines = p.stdout.decode().split()
    ok = [l for l in p.stdout.decode().splitlines() if l.startswith("OK ")]
    bad = [l for l in p.stdout.decode().splitlines() if l.startswith("BAD ")]
    return [l.split()[1] for l in ok], bad, p.stderr.decode().strip(), lines


def chime(n_total, start, rng):
    """Windows-ish notification: two decaying partials, not DTMF."""
    x = np.zeros(n_total)
    n = int(0.4 * RATE)
    t = np.arange(n) / RATE
    tone = (0.4 * np.sin(2 * np.pi * 1046.5 * t) + 0.3 * np.sin(2 * np.pi * 1318.5 * t)) * np.exp(-t * 6)
    x[start:start + n] += tone[: max(0, min(n, n_total - start))]
    return x


COMMANDS = [
    ("rgb ff8800", (ks_tx.OP_RGB, [0xFF, 0x88, 0x00, 100])),
    ("rgb 000000 (repeated nibbles)", (ks_tx.OP_RGB, [0, 0, 0, 0])),
    ("rgb ffffff", (ks_tx.OP_RGB, [0xFF, 0xFF, 0xFF, 100])),
    ("rgb 11aa33 @7%", (ks_tx.OP_RGB, [0x11, 0xAA, 0x33, 7])),
    ("on", (ks_tx.OP_POWER, [1])),
    ("off", (ks_tx.OP_POWER, [0])),
    ("effect purple", (ks_tx.OP_EFFECT, [0x89, 40, 100])),
    ("white 200", (ks_tx.OP_WHITE, [200])),
]

SCENARIOS = [
    ("clean", None),
    ("opus 32k", (["-c:a", "libopus", "-b:a", "32k"], "opus", None)),
    ("opus 16k", (["-c:a", "libopus", "-b:a", "16k"], "opus", None)),
    ("opus 12k voip", (["-c:a", "libopus", "-b:a", "12k", "-application", "voip"], "opus", None)),
    ("aac 64k", (["-c:a", "aac", "-b:a", "64k"], "m4a", None)),
    ("mp3 48k", (["-c:a", "libmp3lame", "-b:a", "48k"], "mp3", None)),
    ("opus 24k + 0.7% clock error", (["-c:a", "libopus", "-b:a", "24k"], "opus",
                                       "asetrate=%d,aresample=%d" % (int(RATE * 1.007), RATE))),
]


def main():
    dec, proto = build()
    r = subprocess.run([proto], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1])
    failures = 0 if r.returncode == 0 else 1
    if r.returncode:
        print(r.stdout)

    rng = np.random.default_rng(1)
    for sname, sc in SCENARIOS:
        for gain_db in (0, -20, -45):
            row = []
            for cname, (op, args) in COMMANDS:
                frame = ks_tx.build_frame(op, args)
                mono = np.array(ks_tx.render(frame, repeat=1)) * 10 ** (gain_db / 20)
                pcm = stereo_s16(mono)
                if sc:
                    pcm = ffmpeg_roundtrip(pcm, sc[0], sc[1], sc[2])
                ok, bad, stats, _ = decode(dec, pcm)
                good = ok.count(frame.hex())
                if good != 1 or len(ok) != 1:
                    failures += 1
                    row.append("FAIL[%s: ok=%s bad=%s %s]" % (cname, ok, bad, stats))
            status = "PASS" if not row else " ".join(row)
            print("%-30s gain %3d dB: %s" % (sname, gain_db, status))

    # Noise floor + chime over the first copy: CRC must reject it, repeat must land.
    frame = ks_tx.build_frame(ks_tx.OP_RGB, [0x12, 0x34, 0x56, 80])
    mono = np.array(ks_tx.render(frame, repeat=2))
    first_frame_mid = int((0.31 + 0.4) * RATE)
    noisy = mono + chime(len(mono), first_frame_mid, rng) + rng.normal(0, 10 ** (-40 / 20), len(mono))
    pcm = ffmpeg_roundtrip(stereo_s16(noisy), ["-c:a", "libopus", "-b:a", "24k"], "opus")
    ok, bad, stats, _ = decode(dec, pcm)
    passed = ok.count(frame.hex()) >= 1 and all(o == frame.hex() for o in ok)
    failures += not passed
    print("%-30s: %s (ok=%d bad=%d) %s" % ("chime on copy 1 + noise", "PASS" if passed else "FAIL", len(ok), len(bad), stats))

    # False positives: 60 s of noise, chimes and random tone pairs must yield nothing valid.
    n = 60 * RATE
    junk = rng.normal(0, 0.05, n)
    for s in range(0, n - RATE, RATE // 2):
        junk += chime(n, s, rng) * rng.uniform(0, 1)
    t = np.arange(n) / RATE
    junk += 0.2 * np.sin(2 * np.pi * rng.uniform(300, 2000) * t)
    # random DTMF bursts with random timing: valid symbols, garbage frames
    for _ in range(300):
        start = rng.integers(0, n - RATE)
        nib = rng.integers(0, 16)
        rr, cc = ks_tx.KEYPOS[int(nib)]
        ln = int(rng.integers(30, 120) * RATE / 1000)
        tt = np.arange(ln) / RATE
        junk[start:start + ln] += 0.3 * (np.sin(2 * np.pi * ks_tx.ROWS[rr] * tt) + np.sin(2 * np.pi * ks_tx.COLS[cc] * tt))
    ok, bad, stats, _ = decode(dec, stereo_s16(junk * 0.5))
    failures += len(ok) > 0
    print("%-30s: %s (false OK=%d, rejected=%d) %s" % ("60 s junk", "PASS" if not ok else "FAIL", len(ok), len(bad), stats))

    # Python CLI smoke test
    wav = os.path.join(BUILD, "cli.wav")
    subprocess.run([sys.executable, os.path.join(ROOT, "host", "ks_tx.py"), "--wav", wav, "rgb", "ff8800"],
                   check=True, capture_output=True)
    with open(wav, "rb") as f:
        data = f.read()[44:]
    ok, _, _, _ = decode(dec, data)
    want = ks_tx.build_frame(ks_tx.OP_RGB, [0xFF, 0x88, 0, 100]).hex()
    cli_ok = ok == [want, want]
    failures += not cli_ok
    print("%-30s: %s" % ("ks_tx.py --wav (repeat 2)", "PASS" if cli_ok else "FAIL %s" % ok))

    print("\n%s" % ("ALL PASSED" if not failures else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
