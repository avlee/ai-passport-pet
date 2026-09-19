#!/usr/bin/env python3
"""Generate the application's CJK LVGL fonts and a coverage manifest.

The ESP32-C3 baseline only enables Montserrat, which has no Chinese glyphs, so
every Chinese UI string needs a real font.  Two LvglFontConverter outputs are
produced:

  pet_font_16  body / Codex message text  -- ASCII + GB2312 level 1 and 2
  pet_font_20  title / state labels       -- ASCII + GB2312 level 1

Both are 4 bpp (anti-aliased) and uncompressed, because the pinned LVGL build
does not enable the compressed-font decoder.

A JSON manifest listing the covered code points is written next to the fonts so
tests/test_pet_font_coverage.py can prove that every Chinese character used by
the application is actually inside the generated subsets.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Printable ASCII: Latin letters, digits and the punctuation used by the UI.
ASCII_RANGE = range(0x20, 0x7F)

# CJK punctuation, fullwidth forms and the few typographic marks that appear in
# Chinese prose or in text relayed from a Codex session.
EXTRA_CODEPOINTS = [
    0x00B0,  # degree sign
    0x00B7,  # middle dot
    0x00D7,  # multiplication sign
    0x2013, 0x2014,  # en/em dash
    0x2018, 0x2019, 0x201C, 0x201D,  # curly quotes
    0x2026,  # horizontal ellipsis
    0x2190, 0x2192,  # left/right arrow
    0x2500,  # box drawing
] + list(range(0x3000, 0x3040)) + list(range(0xFF01, 0xFF5F))


def gb2312_hanzi(level: int) -> list[int]:
    """Enumerate the hanzi of GB2312 level 1 (16-55) and/or level 2 (56-87)."""
    if level == 1:
        rows = range(16, 56)
    elif level == 2:
        rows = range(56, 88)
    else:
        raise ValueError(f"unsupported GB2312 level {level}")
    out: list[int] = []
    for row in rows:
        for cell in range(1, 95):
            try:
                char = bytes([0xA0 + row, 0xA0 + cell]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(char) == 1:
                out.append(ord(char))
    return out


def build_charset(levels: tuple[int, ...]) -> list[int]:
    points = set(ASCII_RANGE) | set(EXTRA_CODEPOINTS)
    for level in levels:
        points |= set(gb2312_hanzi(level))
    return sorted(points)


def run_converter(converter: list[str], font: Path, size: int, name: str,
                  points: list[int], out: Path) -> None:
    symbols = "".join(chr(p) for p in points)
    cmd = converter + [
        "--font", str(font),
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-compress",
        "--no-kerning",
        "--lv-include", "lvgl.h",
        "--lv-font-name", name,
        "--symbols", symbols,
        "--output", str(out),
    ]
    print(f"  {name}: {len(points)} code points, size {size}")
    subprocess.run(cmd, check=True, cwd=ROOT)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", type=Path, required=True,
                    help="source TTF/OTF (must allow redistribution)")
    ap.add_argument("--converter", type=Path, required=True,
                    help="path to lv_font_conv.js")
    ap.add_argument("--node", default="node", help="node executable")
    ap.add_argument("--out-dir", type=Path, default=ROOT / "assets" / "fonts")
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    converter = [args.node, str(args.converter)]

    specs = [
        ("pet_font_16", 16, (1, 2)),
        ("pet_font_20", 20, (1,)),
    ]
    manifest: dict[str, dict] = {}
    for name, size, levels in specs:
        points = build_charset(levels)
        out = args.out_dir / f"{name}.c"
        run_converter(converter, args.font, size, name, points, out)
        manifest[name] = {
            "size": size,
            "bpp": 4,
            "gb2312_levels": list(levels),
            "codepoints": points,
            "source": args.font.name,
        }

    manifest_path = args.out_dir / "pet_font_charset.json"
    manifest_path.write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"  manifest -> {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
