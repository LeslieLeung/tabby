#!/usr/bin/env python3
"""Pack GNU Unifont .hex into Tabby's terminal CJK bitmap (cjk16.bin).

The packed file is not committed. Distribute it as a GitHub Release asset
(see .github/workflows/cjk-font.yml), or generate it locally:

    python3 tools/pack_cjk_font.py -o cjk16.bin

Copy the output to the Tab5 microSD card as /fonts/cjk16.bin. A raw
unifont.hex at /fonts/unifont.hex also works; the firmware parses it at mount.
"""

Format (little-endian):
  magic      4  "CJK1"
  width      2  pixels (16)
  height     2  pixels (16)
  count      4  glyph count
  row_bytes  2  bytes per bitmap row (2)
  reserved   2  0
  codepoints count * uint16  sorted BMP code points
  bitmaps    count * height * row_bytes  1bpp MSB-first, row-major
"""

from __future__ import annotations

import argparse
import gzip
import struct
import sys
import urllib.request
from pathlib import Path

UNIFONT_URL = (
    "https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz"
)

# Wide coverage for a terminal: CJK, kana, hangul, punctuation, fullwidth.
CJK_RANGES = (
    (0x1100, 0x11FF),
    (0x2E80, 0xA4CF),
    (0xA960, 0xA97F),
    (0xAC00, 0xD7A3),
    (0xF900, 0xFAFF),
    (0xFE10, 0xFE19),
    (0xFE30, 0xFE6F),
    (0xFF00, 0xFF60),
    (0xFFE0, 0xFFE6),
)


def in_cjk(cp: int) -> bool:
    return any(lo <= cp <= hi for lo, hi in CJK_RANGES)


def parse_hex(text: str, cjk_only: bool) -> list[tuple[int, bytes]]:
    glyphs: list[tuple[int, bytes]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        left, right = line.split(":", 1)
        try:
            cp = int(left, 16)
        except ValueError:
            continue
        if cp > 0xFFFF:
            continue
        hexbits = "".join(ch for ch in right if ch not in " \t")
        if len(hexbits) != 64:
            continue
        try:
            bitmap = bytes.fromhex(hexbits)
        except ValueError:
            continue
        if len(bitmap) != 32:
            continue
        if cjk_only and not in_cjk(cp):
            continue
        glyphs.append((cp, bitmap))
    glyphs.sort(key=lambda item: item[0])
    deduped: list[tuple[int, bytes]] = []
    seen: set[int] = set()
    for cp, bitmap in glyphs:
        if cp in seen:
            continue
        seen.add(cp)
        deduped.append((cp, bitmap))
    return deduped


def write_bin(path: Path, glyphs: list[tuple[int, bytes]]) -> None:
    header = struct.pack("<4sHHIHH", b"CJK1", 16, 16, len(glyphs), 2, 0)
    codepoints = b"".join(struct.pack("<H", cp) for cp, _ in glyphs)
    bitmaps = b"".join(bitmap for _, bitmap in glyphs)
    path.write_bytes(header + codepoints + bitmaps)


def load_input(path: Path | None) -> str:
    if path is None:
        print(f"downloading {UNIFONT_URL}", file=sys.stderr)
        with urllib.request.urlopen(UNIFONT_URL) as response:
            payload = response.read()
        return gzip.decompress(payload).decode("ascii", errors="ignore")
    data = path.read_bytes()
    if path.suffix == ".gz" or data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data.decode("ascii", errors="ignore")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hex", nargs="?", type=Path, help="unifont .hex or .hex.gz (download if omitted)")
    parser.add_argument("-o", "--output", type=Path, default=Path("cjk16.bin"))
    parser.add_argument("--all", action="store_true", help="keep every 16x16 BMP glyph, not only CJK ranges")
    args = parser.parse_args()
    glyphs = parse_hex(load_input(args.hex), cjk_only=not args.all)
    if not glyphs:
        print("no 16x16 glyphs found", file=sys.stderr)
        return 1
    write_bin(args.output, glyphs)
    size = args.output.stat().st_size
    print(f"wrote {args.output} ({len(glyphs)} glyphs, {size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
