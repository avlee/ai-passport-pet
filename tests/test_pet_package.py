#!/usr/bin/env python3
"""生成器与设备解析器的互校。

.pet 的字节布局是**跨语言契约**: tools/gen_pet_package.py 按 struct 格式写字节,
设备按 main/pet_pkg.h 里的结构体偏移读。两边错开了既不会编译失败, 也不会运行
报错 —— 只会让设备静默拒收(pet_pkg_parse 返回错误), 或者更糟: 通过了结构检查
但帧表偏移整体错位, 画出来是乱的。

所以这里不"对着文档核对", 而是**让生成器真正写出来的字节走一遍设备的解析器**
(tests/parse_pet_package.c, 链接的是 main/pet_pkg.c), 再逐字段比对:

  1. 头部每个字段、每一条状态、每一帧的六元组都要与生成器算出来的一致;
  2. 三条 CRC 必须刚好覆盖各自那一块 —— 逐字节翻转再确认它真的会报错,
     否则"有校验"只是句空话(改错了地方却没人拦, 才是最坏的情况);
  3. 生成器的动画行顺序要与 main/pet_state.h 的 pet_anim_t 一致(C 侧会核对名字);

第 2 条尤其重要: 表区(帧表 + 状态表)是特意排成连续一整段的, 一条 CRC 覆盖两块。
状态表一旦没有校验, 出问题时的表现最难从画面上看出来。

不需要 Pillow: 只走 assemble_package() 那一步。带 Pillow 时会额外跑一遍完整的
"从合成图集切帧"路径。

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
sys.path.insert(0, str(REPO_ROOT / "tools"))

import gen_pet_package as gen  # noqa: E402  (path set up above)


# --------------------------------------------------------------------------
# 设备侧解析器挂具
# --------------------------------------------------------------------------

def build_device_parser(workdir: str) -> str:
    cc = os.environ.get("CC", "cc")
    exe = os.path.join(workdir, "parse_pet_package")
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{REPO_ROOT / 'main'}",
         str(REPO_ROOT / "tests" / "parse_pet_package.c"),
         str(REPO_ROOT / "main" / "pet_pkg.c"),
         str(REPO_ROOT / "main" / "pet_state.c"),
         "-o", exe],
        check=True,
    )
    return exe


def device_parse(exe: str, package: bytes, workdir: str) -> dict:
    """把包写到临时文件, 交给设备解析器, 返回它读出来的东西。"""
    path = Path(workdir) / "candidate.pet"
    path.write_bytes(package)
    proc = subprocess.run([exe, str(path)], capture_output=True)

    lines = proc.stdout.decode("utf-8").splitlines()
    if not lines:
        raise AssertionError(
            f"设备解析器没有输出(stderr={proc.stderr.decode()!r})")

    parts = lines[0].split(" ")
    if parts[0] == "error":
        # 原因本身可能带空格(如 "header crc"), 所以要把剩下的全接回去。
        return {"error": " ".join(parts[1:]), "states": [], "frames": []}

    _, pet_id, display, frames, states, cell_w, cell_h, \
        stage_x, stage_y, stage_w, stage_h, blob_size, total_size = parts

    result = {
        "error": None, "pet_id": pet_id, "display": display,
        "frame_count": int(frames), "state_count": int(states),
        "cell": (int(cell_w), int(cell_h)),
        "stage": (int(stage_x), int(stage_y), int(stage_w), int(stage_h)),
        "blob_size": int(blob_size), "total_size": int(total_size),
        "states": [], "frames": [],
    }
    for line in lines[1:]:
        kind, *rest = line.split(" ")
        if kind == "state":
            result["states"].append((rest[0], int(rest[1]), int(rest[2])))
        elif kind == "frame":
            result["frames"].append(tuple(int(v) for v in rest))
    return result


# --------------------------------------------------------------------------
# 合成一只宠物(不依赖图集)
# --------------------------------------------------------------------------

def synth_pet(seed: int = 0):
    """造一份几何上合法、但与真实宠物无关的帧表 + 像素。

    刻意让每帧的裁剪区都不相同, 这样 stage 并集、x/y 偏移、对齐补齐都会被真正
    走一遍 —— 用全同的帧会让好几处错误都测不出来。
    """
    frames: list[dict] = []
    states: list[dict] = []
    blob = bytearray()
    first = 0

    for index, (name, _row, count) in enumerate(gen.STATES):
        for col in range(count):
            w = 20 + (index * 3 + col) % 17
            h = 24 + (index * 5 + col) % 13
            x = 4 + (index * 7 + col * 11) % 40
            y = 2 + (index * 9 + col * 5) % 30

            blob += b"\x00" * ((-len(blob)) % 4)
            offset = len(blob)
            # 确定性但不像纯色的内容: 逐字节 CRC 才有意义。
            payload = bytes(((offset + i * 31 + seed) % 251) for i in range(w * h * 3))
            blob += payload

            frames.append({"offset": offset, "x": x, "y": y, "w": w, "h": h,
                           "duration": 100 + col * 10})
        states.append({"name": name, "first": first, "count": count})
        first += count

    return bytes(blob), frames, states


def expect_ok(result: dict, where: str) -> None:
    if result["error"] is not None:
        raise AssertionError(f"{where}: 设备解析器拒收了这个包: {result['error']}")


# --------------------------------------------------------------------------
# 用例
# --------------------------------------------------------------------------

def test_layout_matches(tmpdir: str, exe: str) -> None:
    """生成器写的每个字段, 设备都得原样读出来。"""
    blob, frames, states = synth_pet()
    package, stats = gen.assemble_package("test-pet", "测试宠物", blob, frames, states)

    result = device_parse(exe, package, tmpdir)
    expect_ok(result, "layout")

    assert result["pet_id"] == "test-pet", result["pet_id"]
    assert result["display"] == "测试宠物", result["display"]
    assert result["frame_count"] == len(frames)
    assert result["state_count"] == len(states)
    assert result["cell"] == (gen.CELL_W, gen.CELL_H)
    assert result["blob_size"] == len(blob)
    assert result["total_size"] == len(package) == stats["total"]
    assert result["stage"] == stats["stage"], (result["stage"], stats["stage"])

    expected_states = [(s["name"], s["first"], s["count"]) for s in states]
    assert result["states"] == expected_states, result["states"]

    expected_frames = [(f["offset"], f["x"], f["y"], f["w"], f["h"], f["duration"])
                       for f in frames]
    assert result["frames"] == expected_frames

    # 动画行顺序必须与 main/pet_state.h 的 pet_anim_t 一致 —— C 侧按名字核对, 所以
    # 这里读一遍那份枚举, 免得 Python 这边偷偷改了顺序。
    anim_names = read_anim_names()
    assert [s["name"] for s in states] == anim_names, (
        f"生成器的动画行顺序 {[s['name'] for s in states]} "
        f"与 pet_anim_t {anim_names} 不一致")


def test_offsets_are_derived_not_hardcoded(tmpdir: str, exe: str) -> None:
    """两张表紧跟在头部之后、blob 紧跟表区, 且全部 4 字节对齐。"""
    blob, frames, states = synth_pet()
    _, stats = gen.assemble_package("x", "x", blob, frames, states)

    assert stats["frames_offset"] == gen.HEADER_SIZE
    assert stats["states_offset"] == gen.HEADER_SIZE + len(frames) * 16
    assert stats["blob_offset"] == stats["states_offset"] + len(states) * 20
    assert stats["blob_offset"] % 4 == 0, "blob 必须 4 字节对齐"
    assert all(f["offset"] % 4 == 0 for f in frames), "每帧像素必须 4 字节对齐"


def error_name(constant: str) -> str:
    """查枚举量在 pet_pkg_error_name() 里对应的字符串。

    测试里只用 PET_PKG_ERR_* 这些稳定的枚举名, 不去硬编码人类可读的措辞 ——
    改文案不该让测试红。
    """
    source = (REPO_ROOT / "main" / "pet_pkg.c").read_text(encoding="utf-8")
    match = re.search(rf"case {constant}:\s*return \"([^\"]*)\"", source)
    if match is None:
        raise AssertionError(f"main/pet_pkg.c 里找不到 {constant}")
    return match.group(1)


def test_crc_actually_guards_each_region(tmpdir: str, exe: str) -> None:
    """三条 CRC 必须各自守住自己那一块。

    这是"有校验"与"校验对了地方"的区别: 状态表错位是最难从画面上看出来的故障,
    而表区排成连续一段正是为了让它也被覆盖到。逐字节翻转一一确认。
    """
    blob, frames, states = synth_pet()
    package, stats = gen.assemble_package("test-pet", "x", blob, frames, states)
    expect_ok(device_parse(exe, package, tmpdir), "baseline")

    # (说明, 字节位置, 期望的错误)
    # 位置全部取"字段中间", 避开长度字段被改成更小的值而走出另一条错误路径。
    cases = [
        ("头部 CRC 字段本身", gen.HEADER_SIZE - 2, "PET_PKG_ERR_HEADER_CRC"),
        ("pet_id 中间", 52 + 1, "PET_PKG_ERR_HEADER_CRC"),
        ("帧表中间", stats["frames_offset"] + 8, "PET_PKG_ERR_TABLES_CRC"),
        ("状态表中间(表区尾部)",
         stats["states_offset"] + len(states) * 20 - 6, "PET_PKG_ERR_TABLES_CRC"),
        ("blob 中间", stats["blob_offset"] + len(blob) // 2, "PET_PKG_ERR_BLOB_CRC"),
    ]

    for label, offset, constant in cases:
        damaged = bytearray(package)
        damaged[offset] ^= 0xFF
        result = device_parse(exe, bytes(damaged), tmpdir)

        assert result["error"] is not None, f"{label}: 翻转一个字节竟然还能通过"
        expected = error_name(constant)
        assert result["error"] == expected, (
            f"{label}(偏移 {offset}): 期望 {constant}({expected!r}), "
            f"实得 {result['error']!r}")


def test_truncation_and_junk_are_rejected(tmpdir: str, exe: str) -> None:
    blob, frames, states = synth_pet()
    package, _ = gen.assemble_package("test-pet", "x", blob, frames, states)

    # 声明长度超出实际提供的字节: 只能靠 mmap 尾巴(0xFF)兜着, 但 0xFF 那一段不是
    # 合法帧数据, 长度对不上就该拒。
    short = package[:-64]
    result = device_parse(exe, short, tmpdir)
    assert result["error"] in {error_name("PET_PKG_ERR_TOTAL_SIZE"),
                               error_name("PET_PKG_ERR_BLOB_CRC")}, result

    # 擦干净的分区: 全 0xFF, 魔术字不对 —— 设备必须报"没有宠物"而不是崩。
    result = device_parse(exe, b"\xff" * 4096, tmpdir)
    assert result["error"] == error_name("PET_PKG_ERR_MAGIC"), result


def test_real_pet_if_available(tmpdir: str, exe: str) -> None:
    """如果有 Pillow 和真的图集, 再跑一遍完整管线。"""
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("  (跳过完整管线: 这个解释器没有 Pillow)")
        return

    pets_dir = Path.home() / ".codex" / "pets"
    candidates = gen.list_pets(pets_dir)
    if not candidates:
        print(f"  (跳过完整管线: {pets_dir} 里没有宠物)")
        return

    for pet_dir in candidates:
        try:
            package, stats = gen.build_package(pet_dir)
        except SystemExit as exc:
            print(f"  (跳过 {pet_dir.name}: {exc})")
            continue

        result = device_parse(exe, package, tmpdir)
        expect_ok(result, str(pet_dir))
        assert result["pet_id"] == stats["id"]
        assert result["stage"] == stats["stage"]

        # 舞台必须落在设备允许的范围内 —— 越界的包会在 pet_atlas_load() 里被拒,
        # 表现是"传上去了但屏幕上还是'还没有宠物'"。
        _, _, stage_w, stage_h = stats["stage"]
        assert gen.STAGE_W_MIN <= stage_w <= gen.STAGE_W_MAX, stage_w
        assert gen.STAGE_H_MIN <= stage_h <= gen.STAGE_H_MAX, stage_h
        print(f"  {stats['id']}: {stats['frames']} 帧, "
              f"{stats['total'] / 1024 / 1024:.2f} MiB, 舞台 {stage_w}x{stage_h} -> OK")


def read_anim_names() -> list[str]:
    """从 main/pet_state.h 的 pet_anim_t 里读出行名(小写下划线形式)。

    只有第一个枚举量写了 `= 0`, 其余靠自增 —— 所以不能按 `=` 匹配, 否则只会读到
    一行, 断言看起来"通过"而其实什么都没校验。
    """
    names = []
    for name in re.findall(r"\bPET_ANIM_([A-Z][A-Z_]*)\b", read_anim_block()):
        if name != "COUNT":
            names.append(name.lower())
    return names


def read_anim_block() -> str:
    source = (REPO_ROOT / "main" / "pet_state.h").read_text(encoding="utf-8")
    return re.search(r"typedef enum \{(.*?)\} pet_anim_t;", source, re.S).group(1)


def main() -> int:
    print(f"设备解析器挂具 + 生成器互校({len(gen.STATES)} 个动画行)")

    with tempfile.TemporaryDirectory(prefix="pet-package-") as tmpdir:
        exe = build_device_parser(tmpdir)

        test_layout_matches(tmpdir, exe)
        print("  ok  逐字段布局与设备解析器一致")
        test_offsets_are_derived_not_hardcoded(tmpdir, exe)
        print("  ok  表区/blob 偏移与 4 字节对齐")
        test_crc_actually_guards_each_region(tmpdir, exe)
        print("  ok  头部/表区/像素三条 CRC 各自守住了范围")
        test_truncation_and_junk_are_rejected(tmpdir, exe)
        print("  ok  截断与空白分区被拒")
        test_real_pet_if_available(tmpdir, exe)

    print("Pet package round-trip: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
