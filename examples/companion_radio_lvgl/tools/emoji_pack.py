#!/usr/bin/env python3
"""Build the on-SD color-emoji pack for the CrowPanel LVGL UI.

Converts a directory of Twemoji PNGs (named by codepoint, e.g. 1f600.png,
2764.png) into LVGL "true color + alpha" .bin images that the firmware loads
from the SD card via the imgfont fallback (drive 'S:'). Drop the output folder
onto the card as /emoji.

  emoji_pack.py <twemoji_72x72_dir> <out_dir> [--size 18]

Notes:
  * Single-codepoint emoji only (filenames with '-' = ZWJ/flag sequences are
    skipped; the firmware strips VS16/ZWJ/skin-tones to a base codepoint).
  * Output format matches LV_COLOR_DEPTH=16, LV_COLOR_16_SWAP=0
    (RGB565 little-endian + 1 alpha byte per pixel), cf=LV_IMG_CF_TRUE_COLOR_ALPHA.
"""
import argparse
import os
import struct
from PIL import Image

CF_TRUE_COLOR_ALPHA = 5


def to_bin(src, dst, size):
    im = Image.open(src).convert("RGBA").resize((size, size), Image.LANCZOS)
    w, h = im.size
    header = CF_TRUE_COLOR_ALPHA | (w << 10) | (h << 21)   # lv_img_header_t (4 bytes LE)
    out = bytearray(struct.pack("<I", header))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            c565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += struct.pack("<HB", c565, a)             # RGB565 LE + alpha
    with open(dst, "wb") as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser(description="Twemoji PNGs -> LVGL emoji .bin pack")
    ap.add_argument("src", help="dir of Twemoji 72x72 PNGs")
    ap.add_argument("out", help="output dir (copy to SD as /emoji); per-size subfolders")
    ap.add_argument("--sizes", default="12,14,16,20,28",
                    help="comma px sizes to pre-render (one /emoji/<size>/ folder each)")
    ap.add_argument("--only", help="comma-separated hex codepoints to limit (testing)")
    a = ap.parse_args()
    sizes = [int(s) for s in a.sizes.split(",")]
    only = {s.strip().lower() for s in a.only.split(",")} if a.only else None
    for sz in sizes:
        os.makedirs(os.path.join(a.out, str(sz)), exist_ok=True)
    n = 0
    for fn in sorted(os.listdir(a.src)):
        if not fn.endswith(".png"):
            continue
        cp = fn[:-4].lower()
        if "-" in cp:            # multi-codepoint sequence -> skip (single-codepoint pack)
            continue
        if only and cp not in only:
            continue
        for sz in sizes:
            to_bin(os.path.join(a.src, fn), os.path.join(a.out, str(sz), cp + ".bin"), sz)
        n += 1
    print(f"wrote {n} emoji x {len(sizes)} sizes {sizes} to {a.out}")


if __name__ == "__main__":
    main()
