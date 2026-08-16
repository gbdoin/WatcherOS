#!/usr/bin/env python3
"""Convert every sim RGB565 dump (out_*.565) into a round 412x412 PNG in
docs/shots/. Applies a circular alpha mask (transparent outside the bezel)
plus a thin ring, matching the physical round LCD. The 02_egg / 03_hatch
scenes are also written to the legacy README names in docs/."""
import glob, struct, os
from PIL import Image, ImageDraw
W = H = 412
HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.join(HERE, "..", "docs")
SHOTS = os.path.join(DOCS, "shots")
os.makedirs(SHOTS, exist_ok=True)

LEGACY = {"02_egg": "tama_egg.png", "03_hatch": "tama_baby.png"}

def load565(path):
    with open(path, "rb") as f:
        data = f.read()
    px = struct.unpack("<%dH" % (W*H), data)
    img = Image.new("RGB", (W, H))
    out = img.load()
    for i, v in enumerate(px):
        r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F
        out[i % W, i // W] = (r*255//31, g*255//63, b*255//31)
    return img

def roundify(img):
    mask = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(mask)
    d.ellipse((0, 0, W-1, H-1), fill=255)
    img = img.convert("RGBA"); img.putalpha(mask)
    ring = ImageDraw.Draw(img)
    ring.ellipse((1, 1, W-2, H-2), outline=(60, 60, 66, 255), width=3)
    return img

# sim_screens writes its dumps to the process CWD; prefer those so running
# the pair from any directory stays consistent, falling back to sim/.
srcs = sorted(glob.glob(os.path.join(os.getcwd(), "out_*.565")))
if not srcs and os.getcwd() != HERE:
    srcs = sorted(glob.glob(os.path.join(HERE, "out_*.565")))
    if srcs:
        print("note: converting dumps from %s (none in CWD)" % HERE)
if not srcs:
    raise SystemExit("no out_*.565 dumps found (run ./sim_screens first)")
for src in srcs:
    name = os.path.basename(src)[len("out_"):-len(".565")]
    img = roundify(load565(src))
    p = os.path.join(SHOTS, name + ".png")
    img.save(p)
    print("wrote", os.path.relpath(p, HERE))
    if name in LEGACY:
        p = os.path.join(DOCS, LEGACY[name])
        img.save(p)
        print("wrote", os.path.relpath(p, HERE))
