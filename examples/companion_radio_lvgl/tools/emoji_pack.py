#!/usr/bin/env python3
"""Build the on-SD color-emoji pack for the CrowPanel LVGL UI.

Converts a directory of Twemoji PNGs (named by codepoint, e.g. 1f600.png,
2764.png) into one *bundle* per size: /emoji/<size>.bin. Each bundle is a small
index plus the concatenated LVGL "true color + alpha" images, so the firmware
loads a glyph with a RAM index lookup + a single seek -- no per-file directory
scan (thousands of tiny files were murdering SPI performance).

  emoji_pack.py <twemoji_72x72_dir> <out_dir> [--sizes 12,14,16,20,28]

Drop <out_dir>/emoji/ (the .bin bundles) onto the card as /emoji.

Bundle format (little-endian):
  magic  : 4 bytes  'EMJ1'
  count  : uint32
  index  : count x { cp:uint32, offset:uint32, len:uint32 }   (sorted by cp)
  data   : concatenated images, each = lv_img_header(4) + RGB565LE+alpha
(offset is absolute from the start of the file; matches LV_COLOR_DEPTH=16,
 LV_COLOR_16_SWAP=0, cf=LV_IMG_CF_TRUE_COLOR_ALPHA.)
"""
import argparse
import os
import struct
from PIL import Image

CF_TRUE_COLOR_ALPHA = 5
MAGIC = b"EMJ1"


def render(src, size):
    glyph = Image.open(src).convert("RGBA").resize((size, size), Image.LANCZOS)
    pad = max(1, round(size * 0.08))        # equal transparent margin each side so emoji don't touch text/each other
    im = Image.new("RGBA", (size + 2 * pad, size), (0, 0, 0, 0))
    im.paste(glyph, (pad, 0))
    w, h = im.size
    header = CF_TRUE_COLOR_ALPHA | (w << 10) | (h << 21)
    out = bytearray(struct.pack("<I", header))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            out += struct.pack("<HB", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), a)
    return bytes(out)


def build_bundle(items, out_path, size):
    # items: list of (cp:int, png_path), built sorted by cp
    images = [(cp, render(png, size)) for cp, png in items]
    count = len(images)
    data_off = 8 + count * 12               # magic(4) + count(4) + index(count*12)
    index = bytearray()
    data = bytearray()
    pos = data_off
    for cp, img in images:
        index += struct.pack("<III", cp, pos, len(img))
        data += img
        pos += len(img)
    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", count))
        f.write(index)
        f.write(data)
    return count


def main():
    ap = argparse.ArgumentParser(description="Twemoji PNGs -> per-size LVGL emoji bundles")
    ap.add_argument("src", help="dir of Twemoji 72x72 PNGs")
    ap.add_argument("out", help="output dir; writes <out>/emoji/<size>.bin (copy /emoji to SD)")
    ap.add_argument("--sizes", default="10,12,14,16,18,20,24,28", help="comma px sizes")
    a = ap.parse_args()
    sizes = [int(s) for s in a.sizes.split(",")]
    out_dir = os.path.join(a.out, "emoji")
    os.makedirs(out_dir, exist_ok=True)

    items = []
    for fn in sorted(os.listdir(a.src)):
        if not fn.endswith(".png"):
            continue
        cp = fn[:-4].lower()
        if "-" in cp:                      # single-codepoint only
            continue
        try:
            items.append((int(cp, 16), os.path.join(a.src, fn)))
        except ValueError:
            continue
    items.sort(key=lambda t: t[0])         # index must be sorted by codepoint

    for sz in sizes:
        n = build_bundle(items, os.path.join(out_dir, f"{sz}.bin"), sz)
        print(f"  {sz}.bin: {n} emoji")
    print(f"wrote {len(sizes)} bundles to {out_dir}")


if __name__ == "__main__":
    main()
