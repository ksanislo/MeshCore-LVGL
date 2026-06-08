#!/usr/bin/env bash
# Build the gzip emoji-pack assets for the dedicated GitHub "emoji-<fmt>" release from the prebuilt
# per-size /emoji bundles (emoji_pack.py output). The device downloads + gunzips these to the SD card.
#
#   make-emoji-pack.sh <bundle_dir> <out_dir> [fmt]
#     bundle_dir : dir holding <size>.bin   (e.g. .devtmp/emoji_bundle/emoji)
#     out_dir    : where to write emoji-<fmt>-<size>.bin.gz
#     fmt        : emoji format tag (default emj1; must match EMOJI_PACK_FMT in SdCard.h)
#
# Then publish, e.g.:
#   gh release create emoji-emj1 --repo ksanislo/MeshCore-LVGL --title "Emoji pack (EMJ1)" <out_dir>/*.bin.gz
set -euo pipefail
SRC="${1:?bundle dir}"; OUT="${2:?out dir}"; FMT="${3:-emj1}"
SIZES="10 12 14 16 18 20 24 28"   # keep in sync with EMOJI_SIZES in ui-lvgl/SdCard.h
mkdir -p "$OUT"
for s in $SIZES; do
  in="$SRC/$s.bin"; out="$OUT/emoji-$FMT-$s.bin.gz"
  [ -f "$in" ] || { echo "missing $in" >&2; exit 1; }
  gzip -9 -c "$in" > "$out"
  printf '  %8d -> %8d  %s\n' "$(stat -c%s "$in")" "$(stat -c%s "$out")" "$out"
done
echo "done -> $OUT"
