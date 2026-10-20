#!/usr/bin/env python3
"""N devboxes transmitting at the same time on different channels, mixed, then
through a lossy codec. Reports frames recovered from a single copy each."""
import subprocess, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_tests as R

CODECS = [("opus 32k", ["-c:a", "libopus", "-b:a", "32k"], "opus"),
          ("opus 24k", ["-c:a", "libopus", "-b:a", "24k"], "opus"),
          ("opus 16k", ["-c:a", "libopus", "-b:a", "16k"], "opus"),
          ("opus 12k voip", ["-c:a", "libopus", "-b:a", "12k", "-application", "voip"], "opus"),
          ("aac 64k", ["-c:a", "aac", "-b:a", "64k"], "m4a")]


def trial(dec, rng, nch, codec):
    sigs, want = [], set()
    for ch in range(nch):
        args = [int(x) for x in rng.integers(0, 256, 3)] + [int(rng.integers(0, 101))]
        fr = R.ks_tx.build_frame(2, args)
        want.add((ch, fr.hex()))
        a = np.array(R.ks_tx.render(fr, repeat=1, channel=ch))
        sigs.append(np.concatenate([np.zeros(int(rng.integers(0, int(0.6 * 48000)))), a]))
    mix = np.zeros(max(map(len, sigs)) + 48000)
    for s in sigs:
        mix[:len(s)] += s
    mix /= max(1, nch / 2)
    pcm = R.ffmpeg_roundtrip(R.stereo_s16(mix), codec[1], codec[2])
    out = subprocess.run([dec], input=pcm, capture_output=True).stdout.decode().splitlines()
    got = {(int(l.split()[1]), l.split()[2]) for l in out if l.startswith("OK")}
    return len(want & got), len(want), len(got - want)


def main(trials=20):
    dec, _ = R.build()
    rng = np.random.default_rng(11)
    false_total = 0
    for nch in (1, 2, 3, 4):
        for codec in CODECS:
            good = tot = 0
            for _ in range(trials):
                g, t, f = trial(dec, rng, nch, codec)
                good, tot, false_total = good + g, tot + t, false_total + f
            print(f"{nch} at once  {codec[0]:14s} {good:4d}/{tot:<4d} ({100 * good / tot:5.1f}%)")
    print("false frames:", false_total)


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 20)
