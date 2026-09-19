#!/usr/bin/env python3
"""Check that the provision screen's text still fits on one line.

屏幕只有 240px 宽, 配网状态区 `PROV_STATUS_W` = 200px, 而配网状态标签是允许
换行的 —— 文案一变长就会折成两行, 看起来像布局坏了。这不是假想问题: `在 Mac 上
打开「配置 Wi-Fi」`(207px) 和 `已配对，正在接收 Wi-Fi 信息`(205px) 都曾经压线
换行过, 而它们只差几个像素, 靠眼睛是看不出来的。

宽度必须按 LVGL 的算法算, 不能"大致估一下":

  * 每个字形的步进是 ``(adv_w + 8) >> 4`` (见 lvgl 的 lv_font_fmt_txt.c), 也就是
    **逐字形取整后再相加**。先把所有 adv_w 加起来再除以 16 会偏小, 恰好会把压线的
    文案判成"放得下"。
  * 本项目字体是 --no-kerning 生成的, 且 kern_dsc = NULL / kern_classes = 0,
    所以没有字距调整。
  * 配网页的标签都用 lv_obj_remove_style_all() 建, 内边距为 0, 可用宽度就是框宽。

文案清单来自 tests/dump_pet_strings.c (与字库覆盖测试同一份), 框宽从 main/pet_ui.c
里的布局常量读 —— 那些常量的约定就是写成整数字面量。

This runs on the host and needs no ESP-IDF.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FONT_BY_PX = {16: "pet_font_16", 20: "pet_font_20"}
UI_C = REPO_ROOT / "main" / "pet_ui.c"
LAYOUT_C = REPO_ROOT / "main" / "pet_layout.c"
LAYOUT_H = REPO_ROOT / "main" / "pet_layout.h"
DUMPER = REPO_ROOT / "tests" / "dump_pet_strings.c"

# 布局常量分散在三个文件里: 屏幕尺寸与舞台的上下限在 pet_layout.h, 站台坐标在
# pet_layout.c(那边有主机测试); 顶栏/信息面板/配网页/传输页与宠物无关, 在 pet_ui.c。
# 两边都必须是整数字面量。
CONSTANT_SOURCES = (UI_C, LAYOUT_C, LAYOUT_H)

# 动态文案要按最坏情况量: 设备名最长 8 个十六进制位, IPv4 最长 15 个字符。
WORST_DEVICE_NAME = "CodexPet-FFFFFFFF"
WORST_IP = "255.255.255.255"


def dump_strings() -> dict[str, tuple[int, str]]:
    """编译并运行文案打印器, 返回 {key: (px, text)}。"""
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="pet-textfit-") as workdir:
        exe = Path(workdir) / "dump_pet_strings"
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             f"-I{REPO_ROOT / 'main'}", str(DUMPER), "-o", str(exe)],
            check=True,
        )
        stdout = subprocess.run([str(exe)], check=True,
                                capture_output=True).stdout

    rows: dict[str, tuple[int, str]] = {}
    for line in stdout.decode("utf-8").splitlines():
        if not line:
            continue
        key, px, text = line.split("\t", 2)
        rows[key] = (int(px), text)
    return rows


def load_font(name: str):
    """从生成的字体 C 文件里读出步进表与码点映射。

    lv_font_conv 把 unicode_list 写成**相对 range_start 的偏移**(不是绝对码点),
    这一点很容易看错 —— 按绝对值解会得到"所有汉字都缺字形"的假结论。
    """
    src = (REPO_ROOT / "assets" / "fonts" / f"{name}.c").read_text(encoding="utf-8")

    strides: list[int] = []
    for entry in re.findall(r"\{[^{}]*\.adv_w = \d+[^{}]*\}", src):
        match = re.search(r"\.adv_w = (\d+)", entry)
        if match:
            strides.append((int(match.group(1)) + 8) >> 4)

    tables: dict[str, list[int]] = {}
    for table, body in re.findall(
            r"static const (?:uint16_t|uint8_t) (\w+)\[\] = \{(.*?)\};", src, re.S):
        tables[table] = [int(v, 0)
                         for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]

    block = re.search(r"cmaps\[\] =\s*\{(.*?)\n\};", src, re.S).group(1)
    cmaps = []
    for entry in re.findall(
            r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
            r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = (\d+), "
            r"\.type = (\w+)", block):
        cmaps.append((int(entry[0]), int(entry[1]), int(entry[2]),
                      entry[3], entry[4]))
    return strides, cmaps, tables


def glyph_index(cp: int, cmaps, tables) -> int | None:
    for start, length, gid0, ulist, ofs in cmaps:
        if not start <= cp < start + length:
            continue
        rel = cp - start
        if ulist != "NULL":
            lst = tables[ulist]
            return gid0 + lst.index(rel) if rel in lst else None
        if ofs != "NULL":
            value = tables[ofs][rel]
            return gid0 + value if value else None   # 0 表示这一段没有这个字形
        return gid0 + rel
    return None


def measure(text: str, font) -> int:
    strides, cmaps, tables = font
    total = 0
    for ch in text:
        gid = glyph_index(ord(ch), cmaps, tables)
        if gid is None or gid >= len(strides):
            raise AssertionError(f"字库缺字形: {ch!r} (U+{ord(ch):04X})")
        total += strides[gid]
    return total


def ui_constant(name: str) -> int:
    """读布局常量(必须是整数字面量), 依次在 pet_ui.c 与 pet_layout.c 里找。"""
    for path in CONSTANT_SOURCES:
        src = path.read_text(encoding="utf-8")
        match = re.search(rf"^#define {name}\s+(\d+)\s*(?://.*)?$", src, re.M)
        if match is not None:
            return int(match.group(1))
    raise AssertionError(
        f"找不到整数常量 {name} —— 布局常量必须写成整数字面量, "
        f"预览工具和本测试都靠正则读它(找过 "
        f"{', '.join(p.name for p in CONSTANT_SOURCES)})")


def main() -> int:
    strings = dump_strings()
    fonts = {px: load_font(name) for px, name in FONT_BY_PX.items()}

    status_w = ui_constant("PROV_STATUS_W")
    card_w = ui_constant("PROV_CARD_W")
    scr_w = ui_constant("PET_LAYOUT_SCR_W")
    trans_msg_w = ui_constant("TRANS_MSG_W")
    plat_text_w = ui_constant("PLAT_TEXT_W")
    stage_w_ref = ui_constant("PET_LAYOUT_STAGE_W_REF")

    # (说明, 框宽, 字号, 文案) —— 文案的拼装方式与 main/pet_app.c 的
    # on_provision_state() 保持一致(配网完成那条是 "文案 + 空格 + IP")。
    done_px, done_text = strings["prov_done"]
    checks = [
        ("等待开窗", status_w, *strings["prov_waiting"]),
        ("已连接", status_w, *strings["prov_connected"]),
        ("已配对", status_w, *strings["prov_paired"]),
        ("正在联网", status_w, *strings["prov_applying"]),
        ("正在保存", status_w, *strings["prov_saving"]),
        ("配网失败", status_w, *strings["prov_failed"]),
        ("保存失败", status_w, *strings["prov_err_save"]),
        ("连不上", status_w, *strings["prov_err_connect"]),
        ("配网完成+最长IP", status_w, done_px, f"{done_text} {WORST_IP}"),
        ("配对码", card_w, 20, "1 2 3 4"),
        ("配对码标签", card_w, *strings["prov_pin_label"]),
        ("标题", scr_w, *strings["prov_title"]),
        ("设备名(最长)", scr_w, 16, WORST_DEVICE_NAME),
        ("底部提示", scr_w, *strings["prov_hint"]),
        ("底部提示(未开窗)", scr_w, *strings["prov_hint_open"]),
        # 空槽的画面: 舞台区那句要放得进舞台, 站台那句是两行标签(PLAT_TEXT_W)。
        ("没有宠物(舞台)", stage_w_ref, *strings["no_pet"]),
        ("没有宠物(站台)", plat_text_w, *strings["ph_no_pet"]),
        # 传输页: 标题占满整屏, 三句说明落在 TRANS_MSG_W 里(允许换行, 但单句要放得下)。
        ("传输页标题", scr_w, *strings["transfer_title"]),
        ("传输页: 启用中", trans_msg_w, *strings["transfer_enabling"]),
        ("传输页: 失败", trans_msg_w, *strings["transfer_failed"]),
        ("传输页: 重发", trans_msg_w, *strings["transfer_again"]),
    ]

    failures = []
    width = max(len(c[0]) for c in checks)
    for label, box, px, text in checks:
        used = measure(text, fonts[px])
        mark = "ok" if used <= box else "OVER"
        print(f"  [{mark:>4}] {label:<{width}}  {used:>4}px / {box}px  "
              f"({px}px 字体)  {text}")
        if used > box:
            failures.append((label, box, used, text))

    if failures:
        print("\nFAIL: 以下文案会被折行(先减字或换短词, 别去调框宽):",
              file=sys.stderr)
        for label, box, used, text in failures:
            print(f"  {label}: {used}px > {box}px  {text}", file=sys.stderr)
        return 1

    print(f"\n配网界面文案: PASS ({len(checks)} 条都在单行内)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
