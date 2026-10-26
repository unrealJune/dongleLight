#!/usr/bin/env python3
"""Build the portable C core and push ks_tx.py audio through it.

Scenarios approximate what RDP audio redirection might do: lossy codecs at low
bitrates, gain changes, noise, a notification chime landing mid-frame, and a
small clock/pitch error. Needs gcc, ffmpeg (with libopus/aac/mp3), numpy.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORE = os.path.join(ROOT, "firmware", "components", "kscore")
sys.path.insert(0, os.path.join(ROOT, "host"))
import ks as ks_tx  # noqa: E402

BUILD = tempfile.mkdtemp(prefix="ksa_build_")
RATE = 48000
CC = [sys.executable, "-m", "ziglang", "cc"] if os.name == "nt" else ["gcc"]


def executable(name):
    return os.path.join(BUILD, name + (".exe" if os.name == "nt" else ""))


def build(duplex_only=False):
    srcs = [os.path.join(CORE, f) for f in ("ksa_dtmf_rx.c", "ksa_frame.c", "ksa_protocol.c", "ksb_logic.c", "ksc_card.c", "ksbus.c")]
    inc = ["-I", os.path.join(CORE, "include")]
    flags = ["-O2", "-Wall", "-Wextra", "-Werror", "-std=c11"]
    names = ("bus_harness",) if duplex_only else (
        "host_decode", "test_protocol", "test_logic", "card_harness", "bus_harness")
    for name in names:
        subprocess.run([*CC, *flags, *inc, os.path.join(HERE, name + ".c"), *srcs, "-lm",
                        "-o", executable(name)], check=True)
    return executable("host_decode"), executable("test_protocol")


def card_tests():
    """Drive the virtual bridge with APDUs built by ks.py; parse with ks.py."""
    K = ks_tx
    sel_piv = bytes.fromhex("00A4040009A00000030800001000")
    script = [
        ("status before SELECT", K.apdu_status(), "6985"),
        ("SELECT bridge", K.apdu_select(), "019000"),
        ("signal needs-you", K.apdu_submit(K.build_body(K.OP_SIGNAL, K.signal_args("needs-you", "devbox-1"))), "00019000"),
        ("rgb", K.apdu_submit(K.build_body(K.OP_RGB, [255, 136, 0, 80])), "00029000"),
        ("signal working", K.apdu_submit(K.build_body(K.OP_SIGNAL, K.signal_args("working", "train"))), "00039000"),
        ("bad op", K.apdu_submit(bytes([0x5E, 1])), "6a80"),
        ("Windows probes PIV", sel_piv, "6a82"),
        ("submit after foreign SELECT", K.apdu_submit(K.build_body(K.OP_ACK, [])), "6985"),
        ("reSELECT", K.apdu_select(), "019000"),
        ("status", K.apdu_status(), None),
    ]
    raw = [
        ("power on returns ATR", "62000000000000000000"),
        ("bad slot fails", "65000000000100000000"),
        ("get parameters (T=1)", "6c000000000000000000"),
        ("escape unsupported", "6b000000000000000000"),
    ]
    inp = "".join(a.hex() + "\n" for _, a, _ in script) + "".join("!" + m + "\n" for _, m in raw)
    exe = executable("card_harness")
    out = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.split()
    fails = 0
    for (name, _, want), got in zip(script, out):
        if want is not None and got != want:
            print("  FAIL card %-28s got %s want %s" % (name, got, want))
            fails += 1
    st = K.parse_status(bytes.fromhex(out[len(script) - 1])[:-2])
    names = {s["name"]: (s["state"], s["lamp"]) for s in st["sources"]}
    lamps = [(l["name"], l["connected"], l["readback"], l["verified"]) for l in st["lamps"]]
    if (names != {"devbox-1": ("needs-you", 1), "train": ("working", 2)}
            or lamps != [("Triones-1A2B", True, True, True), ("KS03~77C0", False, False, False)]
            or st["link"] != "connected"):
        print("  FAIL card status parse:", st)
        fails += 1
    r = [bytes.fromhex(x) for x in out[len(script):]]
    atr = r[0][10:]
    tck = 0
    for b in atr[1:-1]:
        tck ^= b
    checks = [r[0][0] == 0x80 and atr[:4] == bytes.fromhex("3b878101") and atr[-1] == tck and b"KSLIGHT" in atr,
              r[1][0] == 0x81 and r[1][7] & 0x40 and r[1][8] == 5,
              r[2][0] == 0x82 and r[2][9] == 1 and len(r[2]) == 17,
              r[3][0] == 0x83 and r[3][7] & 0x40]
    for (name, _), ok in zip(raw, checks):
        if not ok:
            print("  FAIL ccid", name, r)
            fails += 1
    print("%-30s: %s" % ("card applet + CCID", "PASS" if not fails else "FAIL"))
    return fails


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
    return [l.split()[2] for l in ok], bad, p.stderr.decode().strip(), lines


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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duplex-only", action="store_true", help="Build and run only the simulated duplex integration suite.")
    args = parser.parse_args()
    dec, proto = build(args.duplex_only)
    if args.duplex_only:
        return subprocess.run([sys.executable, os.path.join(HERE, "test_duplex_integration.py")],
                              env=dict(os.environ, KSBUS_HARNESS=executable("bus_harness"))).returncode
    r = subprocess.run([proto], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1])
    failures = 0 if r.returncode == 0 else 1
    if r.returncode:
        print(r.stdout)

    r = subprocess.run([executable("test_logic")], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1])
    failures += r.returncode != 0
    failures += card_tests()
    bus_tests = subprocess.run([sys.executable, os.path.join(HERE, "test_audio_bus.py")],
                              env=dict(os.environ, KSBUS_HARNESS=executable("bus_harness")))
    failures += bus_tests.returncode != 0
    integration = subprocess.run([sys.executable, os.path.join(HERE, "test_duplex_integration.py")],
                                env=dict(os.environ, KSBUS_HARNESS=executable("bus_harness")))
    failures += integration.returncode != 0

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
    # random tone-pair bursts on random channels: valid symbols, garbage frames
    for _ in range(300):
        start = rng.integers(0, n - RATE)
        nib = rng.integers(0, 16)
        rr, cc = ks_tx.KEYPOS[int(nib)]
        ch = int(rng.integers(0, ks_tx.MAX_CHANNELS))
        ln = int(rng.integers(30, 120) * RATE / 1000)
        tt = np.arange(ln) / RATE
        junk[start:start + ln] += 0.3 * (np.sin(2 * np.pi * ks_tx.tone_hz(ch, rr) * tt) +
                                         np.sin(2 * np.pi * ks_tx.tone_hz(ch, 4 + cc) * tt))
    ok, bad, stats, _ = decode(dec, stereo_s16(junk * 0.5))
    failures += len(ok) > 0
    print("%-30s: %s (false OK=%d, rejected=%d) %s" % ("60 s junk", "PASS" if not ok else "FAIL", len(ok), len(bad), stats))

    # Python CLI smoke test
    wav = os.path.join(BUILD, "cli.wav")
    subprocess.run([sys.executable, os.path.join(ROOT, "host", "ks.py"), "--wav", wav, "--channel", "0", "rgb", "ff8800"],
                   check=True, capture_output=True)
    with open(wav, "rb") as f:
        data = f.read()[44:]
    ok, _, _, _ = decode(dec, data)
    want = ks_tx.build_frame(ks_tx.OP_RGB, [0xFF, 0x88, 0, 100]).hex()
    cli_ok = ok == [want, want]
    failures += not cli_ok
    print("%-30s: %s" % ("ks.py --wav (repeat 2)", "PASS" if cli_ok else "FAIL %s" % ok))

    # Agent signal via the CLI on every channel
    sig_ok = True
    for ch in range(ks_tx.MAX_CHANNELS):
        wav = os.path.join(BUILD, "sig%d.wav" % ch)
        env = dict(os.environ, KS_SOURCE="box-%d" % ch)
        subprocess.run([sys.executable, os.path.join(ROOT, "host", "ks.py"), "--wav", wav, "--channel", str(ch),
                        "signal", "needs-you"], check=True, capture_output=True, env=env)
        with open(wav, "rb") as f:
            out = subprocess.run([dec], input=f.read()[44:], capture_output=True).stdout.decode()
        want = ks_tx.build_frame(ks_tx.OP_SIGNAL, ks_tx.signal_args("needs-you", "box-%d" % ch)).hex()
        sig_ok &= ("OK %d %s" % (ch, want)) in out
    failures += not sig_ok
    print("%-30s: %s" % ("ks.py signal, channels 0-3", "PASS" if sig_ok else "FAIL"))

    print("\n%s" % ("ALL PASSED" if not failures else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
