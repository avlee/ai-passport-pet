#!/usr/bin/env python3
"""Check that every UI string is covered by the generated CJK fonts.

中文界面必须自备字库: 基线 LVGL 只带 Montserrat, 没有任何汉字。字库是子集化的,
所以新增文案有可能落在子集之外, 表现是屏幕上出现方框(placeholder glyph)。

本测试拿 tests/dump_pet_strings.c 打印出来的界面文案清单, 对照
assets/fonts/pet_font_charset.json(与字库一起生成, 记录每个字体的码点集合)逐个
码点核对。真机侧还有一道同样的启动自检, 见 main/pet_app.c。

This runs on the host and needs no ESP-IDF.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST = REPO_ROOT / "assets" / "fonts" / "pet_font_charset.json"
DUMPER = REPO_ROOT / "tests" / "dump_pet_strings.c"

# 界面字号 -> 字库名。与 main/pet_fonts.c 的映射保持一致。
FONT_BY_PX = {16: "pet_font_16", 20: "pet_font_20"}


def dump_strings() -> list[tuple[str, int, str]]:
    """编译并运行文案清单打印器, 返回 [(key, px, text)]。"""
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="pet-strings-") as workdir:
        exe = Path(workdir) / "dump_pet_strings"
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             f"-I{REPO_ROOT / 'main'}", str(DUMPER), "-o", str(exe)],
            check=True,
        )
        stdout = subprocess.run([str(exe)], check=True,
                                capture_output=True).stdout

    rows: list[tuple[str, int, str]] = []
    for line in stdout.decode("utf-8").splitlines():
        if not line:
            continue
        key, px, text = line.split("\t", 2)
        rows.append((key, int(px), text))
    return rows


def main() -> int:
    if not MANIFEST.exists():
        print(f"FAIL: 缺少字库清单 {MANIFEST.relative_to(REPO_ROOT)}; "
              f"先运行 python3 tools/gen_pet_fonts.py", file=sys.stderr)
        return 1

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    coverage: dict[str, set[int]] = {}
    for font_name, meta in manifest.items():
        coverage[font_name] = set(meta["codepoints"])

    strings = dump_strings()
    print(f"检查 {len(strings)} 条界面文案, 字库 {sorted(coverage)}")

    problems: list[str] = []
    for key, px, text in strings:
        font_name = FONT_BY_PX.get(px)
        if font_name is None:
            problems.append(f"{key}: 未知字号 {px}")
            continue
        if font_name not in coverage:
            problems.append(f"{key}: 清单里没有字体 {font_name}")
            continue

        missing = [
            ch for ch in text
            if ord(ch) >= 0x20 and ord(ch) != 0x7F
            and ord(ch) not in coverage[font_name]
        ]
        if missing:
            detail = ", ".join(f"{ch!r}(U+{ord(ch):04X})" for ch in missing)
            problems.append(f"{key} [{font_name} {px}px] \"{text}\" 缺字: {detail}")

    if problems:
        print("FAIL: 以下文案在字库里没有字形, 真机会显示成方框:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print("修法: 扩充 tools/gen_pet_fonts.py 的字符集后重新生成字库。",
              file=sys.stderr)
        return 1

    print("Font coverage: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
