#!/bin/sh
# Compile the real kscore model + renderer to WebAssembly and inline it into
# lumi.html -> lumi-sim.html. Needs zig (pip install ziglang).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE="$HERE/../../firmware/components/kscore"
OUT=${1:-"$HERE/lumi-sim.html"}
TMP=$(mktemp -d)
python3 -m ziglang cc -target wasm32-wasi -O2 -mexec-model=reactor -I "$CORE/include" \
  -Wl,--export-dynamic -Wl,--strip-all \
  "$HERE/sim.c" "$CORE/ksb_logic.c" "$CORE/ksa_frame.c" "$CORE/ksa_protocol.c" \
  "$CORE/ksui_render.c" "$CORE/ksui_font.c" -o "$TMP/lumi.wasm"
python3 - "$HERE/lumi.html" "$TMP/lumi.wasm" "$OUT" <<'PY'
import base64, sys
page, wasm, out = sys.argv[1:]
html = open(page).read().replace("__LUMI_WASM_BASE64__", base64.b64encode(open(wasm, "rb").read()).decode())
open(out, "w").write(html)
print("wrote", out, len(html), "bytes")
PY
