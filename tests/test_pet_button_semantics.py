#!/usr/bin/env python3
"""Check that the provision screen's hint and the button handler agree.

配网页底下写着"长按下键退出配网", 但长按下键的处理分支早先是**只开不关**的: 它无条件
调用 `open_provision_window()`, 而后者发现窗口已经开着就直接 `return`。用户照着屏幕上
的提示去关, 却什么也没发生 —— 屏幕被不透明的配网页盖着, 宠物也看不见, 唯一合理的
结论就是"死机了"。真机上就是这么被报上来的。

这类 bug 不会崩、不会报错、也不违反任何类型约束: 它只是"界面许诺了一件事, 代码不会做
那件事"。编译器看不出来, 唯一能钉住它的地方就是把两边放在一起断言 —— 这条测试干的就是
这件事:

  * 提示语里出现"退出" → DOWN 长按分支必须能调用 `pet_provision_stop()`;
  * 同时必须仍然调用 `open_provision_window()` —— 下键是**开关**, 不是单向的,
    否则又变成"关得掉但开不回来"。

纯源码检查, 不需要 ESP-IDF, 也不需要 LVGL。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
STRINGS_H = REPO_ROOT / "main" / "pet_strings.h"
APP_C = REPO_ROOT / "main" / "pet_app.c"


def string_macro(path: Path, name: str) -> str | None:
    """读 `#define NAME \"...\"` 的字面量; 找不到返回 None。"""
    match = re.search(
        rf'^#define\s+{name}\s+"([^"]*)"', path.read_text(encoding="utf-8"), re.M
    )
    return match.group(1) if match else None


def long_press_down_branch(source: str) -> str:
    """取出长按分支里 DOWN 的那一段: 从 `BSP_BTN_LONG` 到其后的 `BSP_BTN_UP`。

    这样就只盯着"长按下键"这一条路径, 不会把配网页起来之后那条
    `if (pet_provision_active()) return;` 早退逻辑误当成按键处理。
    """
    start = source.index("BSP_BTN_LONG")
    end = source.index("BSP_BTN_UP", start)
    return source[start:end]


def main() -> int:
    hint = string_macro(STRINGS_H, "PET_STR_PROV_HINT")
    if hint is None:
        print("FAIL: main/pet_strings.h 里找不到 PET_STR_PROV_HINT", file=sys.stderr)
        return 1

    source = APP_C.read_text(encoding="utf-8")
    try:
        branch = long_press_down_branch(source)
    except ValueError as exc:
        print(f"FAIL: main/pet_app.c 里找不到长按分支: {exc}", file=sys.stderr)
        return 1

    promises_exit = "退出" in hint
    can_exit = "pet_provision_stop(" in branch
    can_open = "open_provision_window(" in branch

    print(f"  屏幕提示          PET_STR_PROV_HINT = {hint!r}")
    print(f"  提示承诺能退出    {'是' if promises_exit else '否'}")
    print(f"  代码能关掉配网    {'是' if can_exit else '否'}  (pet_provision_stop)")
    print(f"  代码能打开配网    {'是' if can_open else '否'}  (open_provision_window)")

    failures = []
    if promises_exit and not can_exit:
        failures.append(
            f'配网页提示"{hint}"承诺能退出, 但长按下键分支没有调用 pet_provision_stop()'
        )
    if not can_open:
        failures.append(
            "长按下键分支不再调用 open_provision_window(): "
            "下键必须仍是开关(能开也能关)"
        )

    if failures:
        print("\nFAIL: 屏幕提示与按键处理不一致:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print("\n配网按键语义: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
