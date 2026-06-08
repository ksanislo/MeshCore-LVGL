#!/usr/bin/env python3
"""
Regenerate firmware-manifest.json from the GitHub releases.

The on-device OTA check fetches this tiny file from
  https://raw.githubusercontent.com/<owner>/<repo>/main/firmware-manifest.json
instead of the GitHub releases API. It carries only {ver, sha, pre} per release; the device
CONSTRUCTS its own per-board download URL from its compiled-in OTA_ASSET_PREFIX + ver + sha
(asset names are deterministic: "<prefix>-<ver>-<sha7>.bin"). That keeps the manifest tiny and
board-count-independent -- the verbose multi-board releases JSON is parsed HERE (on a real machine),
never on the device.

Run as part of the release process AFTER the GitHub release exists, then commit the result:
  python3 tools/gen_manifest.py && git add firmware-manifest.json && git commit ...

Requires the `gh` CLI (authenticated).
"""
import json, re, subprocess, pathlib, sys

REPO = "ksanislo/MeshCore-LVGL"
N = 15                                  # most-recent releases to list (picker depth; tiny either way)
SHA_RE = re.compile(r"^[0-9a-f]{7,40}$")

def gh_releases():
    out = subprocess.run(
        ["gh", "api", f"repos/{REPO}/releases?per_page={N}"],
        capture_output=True, text=True, check=True,
    ).stdout
    return json.loads(out)

def sha_from_assets(rel):
    # Any board's app image: "<prefix>-<ver>-<sha7>.bin" (NOT the -merged factory image). The sha7 is
    # always the final '-'-delimited segment before ".bin", regardless of prefix or dashes in <ver>.
    for a in rel.get("assets", []):
        nm = a.get("name", "")
        if nm.endswith(".bin") and "-merged" not in nm:
            seg = nm[:-4].rsplit("-", 1)[-1]
            if SHA_RE.match(seg):
                return seg
    return None

def main():
    releases = []
    for rel in gh_releases():
        tag = rel.get("tag_name", "")
        sha = sha_from_assets(rel)
        if not tag or not sha:
            continue                    # no deterministic per-board asset -> device can't build a URL
        releases.append({"ver": tag, "sha": sha, "pre": bool(rel.get("prerelease"))})

    manifest = {"releases": releases}
    path = pathlib.Path(__file__).resolve().parents[1] / "firmware-manifest.json"
    path.write_text(json.dumps(manifest, separators=(",", ":")) + "\n")
    print(f"wrote {path} ({len(releases)} releases, {path.stat().st_size} bytes)")
    if not releases:
        print("WARNING: no releases with deterministic assets found", file=sys.stderr)

if __name__ == "__main__":
    main()
