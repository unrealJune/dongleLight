#!/usr/bin/env python3
"""Byte-for-byte check of our Triones encoder against the real trionesControl.

    pip download --no-deps --no-binary :all: trionesControl==1.2.0 && tar xzf trionesControl-1.2.0.tar.gz
    python3 test/xcheck_trionescontrol.py trionesControl-1.2.0

pygatt is stubbed out, so no Bluetooth is needed.
"""
import os, random, subprocess, sys, tempfile, types

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(os.path.dirname(HERE), "firmware", "components", "kscore")
SRC = r'''
#include <stdio.h>
#include <stdlib.h>
#include "ksa_protocol.h"
int main(int c, char **v) {
  const ks_profile_t *p = ks_profile_for_name("Triones-X"); uint8_t o[16]; size_t n = 0;
  int a = atoi(v[2]), b = c > 3 ? atoi(v[3]) : 0, d = c > 4 ? atoi(v[4]) : 0;
  if (v[1][0] == 'o') n = ks_pkt_power(p, a, o);
  if (v[1][0] == 'r') n = ks_pkt_rgb(p, a, b, d, 100, o);
  for (size_t i = 0; i < n; i++) printf("%02x", o[i]); printf("\n"); return 0; }
'''


def main(tc_dir):
    tmp = tempfile.mkdtemp()
    with open(os.path.join(tmp, "pkt.c"), "w") as f:
        f.write(SRC)
    exe = os.path.join(tmp, "pkt")
    subprocess.run(["gcc", "-I", os.path.join(CORE, "include"), os.path.join(tmp, "pkt.c"),
                    os.path.join(CORE, "ksa_protocol.c"), "-o", exe], check=True)
    pg, exc = types.ModuleType("pygatt"), types.ModuleType("pygatt.exceptions")
    exc.NotConnectedError = exc.BLEError = type("E", (Exception,), {})
    pg.exceptions = exc
    sys.modules.update({"pygatt": pg, "pygatt.exceptions": exc})
    sys.path.insert(0, tc_dir)
    import trionesControl.trionesControl as tc

    class Dev:
        def char_write(self, uuid, data, wait_for_response=False):
            self.last = bytes(data).hex()

    def ours(*a):
        return subprocess.run([exe, *map(str, a)], capture_output=True, text=True).stdout.strip()

    d, bad, rng = Dev(), 0, random.Random(3)
    for on in (1, 0):
        (tc.powerOn if on else tc.powerOff)(d)
        bad += d.last != ours("o", on)
    for _ in range(200):
        r, g, b = (rng.randrange(256) for _ in range(3))
        tc.setRGB(r, g, b, d)
        bad += d.last != ours("r", r, g, b)
    print("mismatches: %d of 202" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
