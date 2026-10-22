#!/usr/bin/env python3
"""Render the dongle screen on the host: a labelled PNG sheet + an animated GIF."""
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(os.path.dirname(HERE), "firmware", "components", "kscore")
W, H, SCALE = 160, 80, 4
SCENES = ["searching (audio)", "linked, orange", "lamp off: asleep", "devbox-1 needs you",
          "error + new signal", "acked: heart", "purple fx, 6 agents", "2 lamps, 2 devboxes",
          "devbox-2 needs you (lamp 2)", "long-press: claiming", "claimed lamp 2", "fresh: nothing claimed"]


def build():
    out = os.path.join(tempfile.mkdtemp(), "ui_preview")
    srcs = [os.path.join(CORE, f) for f in ("ksui_render.c", "ksui_font.c", "ksb_logic.c", "ksa_frame.c", "ksa_protocol.c")]
    subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-std=c11", "-I", os.path.join(CORE, "include"),
                    os.path.join(HERE, "ui_preview.c"), *srcs, "-lm", "-o", out], check=True)
    return out


def frames(exe, sc, t0, n, step, portrait=False):
    raw = subprocess.run([exe, str(sc), str(t0), str(n), str(step), "p" if portrait else "l"],
                         capture_output=True, check=True).stdout
    a = np.frombuffer(raw, dtype="<u2").reshape((n, W, H) if portrait else (n, H, W))
    rgb = np.stack([((a >> 11) & 31) * 255 // 31, ((a >> 5) & 63) * 255 // 63, (a & 31) * 255 // 31], -1)
    return rgb.astype(np.uint8)


def main(out_dir):
    exe = build()
    os.makedirs(out_dir, exist_ok=True)
    tiles = []
    for sc, name in enumerate(SCENES):
        f = frames(exe, sc, 1000, 1, 1)[0]
        tile = Image.fromarray(f).resize((W * SCALE, H * SCALE), Image.NEAREST)
        canvas = Image.new("RGB", (W * SCALE, H * SCALE + 28), (24, 24, 28))
        canvas.paste(tile, (0, 28))
        ImageDraw.Draw(canvas).text((8, 8), name, fill=(220, 220, 230))
        tiles.append(canvas)
    cols = 2
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W * SCALE + 12), rows * (H * SCALE + 40)), (12, 12, 14))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * (W * SCALE + 12), (i // cols) * (H * SCALE + 40)))
    sheet.save(os.path.join(out_dir, "screen_states.png"))

    # GIF: a little story, 2.4 s per scene at 20 fps
    gif = []
    for sc in (7, 8, 5, 9, 10, 2):
        for f in frames(exe, sc, 0, 48, 50):
            gif.append(Image.fromarray(f).resize((W * 3, H * 3), Image.NEAREST))
    gif[0].save(os.path.join(out_dir, "lumi.gif"), save_all=True, append_images=gif[1:], duration=50, loop=0)

    # Portrait (the dongle standing with its plug away from you): sheet + GIF
    PS = 3
    tiles = []
    for sc, name in enumerate(SCENES):
        f = frames(exe, sc, 1000, 1, 1, portrait=True)[0]
        tile = Image.fromarray(f).resize((H * PS, W * PS), Image.NEAREST)
        canvas = Image.new("RGB", (H * PS + 20, W * PS + 40), (12, 12, 14))
        canvas.paste(tile, (10, 34))
        ImageDraw.Draw(canvas).text((10, 10), name[:30], fill=(220, 220, 230))
        tiles.append(canvas)
    cols = 6
    tw, th = tiles[0].size
    sheet = Image.new("RGB", (cols * tw, ((len(tiles) + cols - 1) // cols) * th), (12, 12, 14))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * tw, (i // cols) * th))
    sheet.save(os.path.join(out_dir, "screen_states_portrait.png"))
    gif = []
    for sc in (7, 8, 5, 9, 10, 2):
        for f in frames(exe, sc, 0, 48, 50, portrait=True):
            gif.append(Image.fromarray(f).resize((H * PS, W * PS), Image.NEAREST))
    gif[0].save(os.path.join(out_dir, "lumi_portrait.gif"), save_all=True, append_images=gif[1:], duration=50, loop=0)
    print("wrote", out_dir)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "preview")
