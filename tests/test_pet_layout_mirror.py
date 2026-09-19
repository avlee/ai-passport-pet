#!/usr/bin/env python3
"""预览工具的版式算术必须与设备一致。

tools/preview_pet_screen.py 跑在编辑器里, 编译不了 C, 所以它把 pet_layout_for_stage()
在 Python 里又实现了一遍。两份实现跑偏之后的后果是"预览看着居中、真机偏几像素" ——
两边都不报任何错, 而预览恰恰是编辑器里唯一能看见那块屏的手段: 它一旦不可信, 就没有
替代品了。

所以这里拿 C 的真值(tests/dump_pet_layout.c)逐字段对照 Python 的镜像:

  * 每个字段都要对, 不只是 stage_*; 站台文本块那些派生值同样会跑偏;
  * 两个下限/两个上限/被拒的尺寸都要一致 —— 镜像的"拒"必须与设备的"拒"同一条边界;
  * 顺带把 .pet 打包器里的舞台上下限也与设备对一遍(它也自己抄了一份)。

不需要 Pillow: 只借走 preview_pet_screen 的版式算术, 图画那部分不碰。

This runs on the host and needs no ESP-IDF.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import gen_pet_package as gen            # noqa: E402  (路径在上面设好了)
import preview_pet_screen as preview     # noqa: E402

CONSTS = preview.parse_defines(preview.CONSTANT_SOURCES)

# 覆盖几个角: 参照宠物(sophie-portrait)、更宽的 li-muwan、两个下限、两个上限。
# 206 是脚底固定在 235 那一行时能容纳的最高舞台 —— 再高就把顶边顶进状态行。
CASES = [
    (129, 198), (156, 198),
    (64, 64), (224, 206), (224, 64), (64, 198), (200, 150),
]

# 必须被拒: 宽越界、太窄、太矮、顶边压状态行。
REJECTED = [(63, 198), (225, 198), (129, 63), (129, 207)]


def build_dumper(workdir: str) -> str:
    """编译设备侧的版式打印器 —— 真值就是它吐出来的那几个数。"""
    cc = os.environ.get("CC", "cc")
    exe = os.path.join(workdir, "dump_pet_layout")
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{REPO_ROOT / 'main'}",
         str(REPO_ROOT / "tests" / "dump_pet_layout.c"),
         str(REPO_ROOT / "main" / "pet_layout.c"),
         "-o", exe],
        check=True,
    )
    return exe


def device_layout(exe: str, stage_w: int, stage_h: int) -> dict[str, int] | None:
    stdout = subprocess.run([exe, str(stage_w), str(stage_h)], check=True,
                            capture_output=True).stdout.decode("utf-8")
    stdout = stdout.strip()
    if stdout == "unsupported":
        return None
    return {name: int(value)
            for name, value in (item.split("=") for item in stdout.split())}


def mirror_layout(stage_w: int, stage_h: int) -> dict[str, int] | None:
    try:
        return preview.layout_for_stage(stage_w, stage_h, CONSTS)
    except SystemExit:
        return None


def test_fields_match(exe: str) -> None:
    for stage_w, stage_h in CASES:
        device = device_layout(exe, stage_w, stage_h)
        mirror = mirror_layout(stage_w, stage_h)

        assert device is not None, f"{stage_w}x{stage_h}: 设备说放得下, 镜像却拒了"
        assert mirror is not None, f"{stage_w}x{stage_h}: 镜像说放得下, 设备却拒了"

        missing = set(device) - set(mirror)
        extra = set(mirror) - set(device)
        assert not missing and not extra, (
            f"{stage_w}x{stage_h}: 字段集合不同, 镜像缺 {sorted(missing)}, "
            f"多 {sorted(extra)}")

        for name in sorted(device):
            assert device[name] == mirror[name], (
                f"{stage_w}x{stage_h}: {name} 设备={device[name]} 镜像={mirror[name]}")
        print(f"  ok  {stage_w}x{stage_h}: 舞台 @({mirror['stage_x']},{mirror['stage_y']}), "
              f"文本块 @({mirror['plat_text_x']},{mirror['plat_text_y']})")


def test_rejections_match(exe: str) -> None:
    for stage_w, stage_h in REJECTED:
        device = device_layout(exe, stage_w, stage_h)
        mirror = mirror_layout(stage_w, stage_h)
        assert device is None, f"{stage_w}x{stage_h}: 设备接受了不该接受的尺寸"
        assert mirror is None, f"{stage_w}x{stage_h}: 镜像接受了不该接受的尺寸"
    print(f"  ok  {len(REJECTED)} 个越界尺寸两边都拒")


def test_package_bounds_match(exe: str) -> None:
    """打包器的舞台上下限必须与设备一致, 否则会打出一份设备拒收的包。"""
    for name, limit, other in (("宽", gen.STAGE_W_MIN, gen.STAGE_W_MAX),
                               ("高", gen.STAGE_H_MIN, gen.STAGE_H_MAX)):
        candidates = (limit - 1, limit, other, other + 1)
        for value in candidates:
            expected = limit <= value <= other
            actual = (device_layout(exe, value, 198) if name == "宽"
                      else device_layout(exe, 129, value)) is not None
            assert actual == expected, (
                f"舞台{name} {value}: 打包器认为{'可用' if expected else '越界'}, "
                f"设备说{'可用' if actual else '越界'}")
    print(f"  ok  打包器的舞台范围 {gen.STAGE_W_MIN}..{gen.STAGE_W_MAX} x "
          f"{gen.STAGE_H_MIN}..{gen.STAGE_H_MAX} 与设备一致")


def device_frame_rect(exe: str, stage_w: int, stage_h: int,
                      frame: tuple[int, int, int, int],
                      crop: tuple[int, int]) -> dict[str, int] | str:
    args = [exe, str(stage_w), str(stage_h),
            str(frame[0]), str(frame[1]), str(frame[2]), str(frame[3]),
            str(crop[0]), str(crop[1])]
    stdout = subprocess.run(args, check=True, capture_output=True).stdout
    stdout = stdout.decode("utf-8").strip()
    if stdout in ("unsupported", "rejected"):
        return stdout
    # 设备侧打的是 sprite_x/sprite_y/..., 这里去掉前缀好跟镜像的 key 直接比。
    return {name.removeprefix("sprite_"): int(value)
            for name, value in (item.split("=") for item in stdout.split())}


def mirror_frame_rect(stage_w: int, stage_h: int,
                      frame: tuple[int, int, int, int],
                      crop: tuple[int, int]) -> dict[str, int] | str:
    """镜像侧只借算术, 不碰画图 —— 与 preview.frame_rect() 同一份实现。

    包头的舞台是 (crop_x0, crop_y0, stage_w, stage_h): 尺寸与版式用的是同一个值
    (都来自包头), 所以这里把后两个用参数补上, 于是 C 侧"帧的裁剪框必须含在舞台
    裁剪框里"这道判断在两边是同一个式子。
    """
    f = {"x": frame[0], "y": frame[1], "w": frame[2], "h": frame[3]}
    try:
        return preview.frame_rect(f, (crop[0], crop[1], stage_w, stage_h))
    except SystemExit:
        return "rejected"


def test_frame_landing_matches(exe: str) -> None:
    """帧的落点算式也必须两边一致。

    这条是补上来的: 设备侧曾经拿**屏幕上**的舞台落点当裁剪原点去减, 于是每帧都
    左上偏几十像素并被舞台裁掉一角 —— 预览画得对, 真机不对, 而当时的镜像测试只
    比对了版式字段, 落点这条式子根本没有对手可比。
    """
    cases = [
        # (舞台尺寸, 帧裁剪框, 舞台裁剪原点)。前两条是参照宠物
        # (sophie-portrait, 并集 129x198 @ (31,5)) 的真值。
        ((129, 198), (31, 5, 129, 198), (31, 5)),
        ((129, 198), (60, 24, 40, 40), (31, 5)),
        ((129, 198), (152, 195, 8, 8), (31, 5)),
        ((156, 198), (18, 7, 156, 198), (18, 7)),
        ((224, 206), (0, 0, 224, 206), (0, 0)),
    ]
    for (stage_w, stage_h), frame, crop in cases:
        device = device_frame_rect(exe, stage_w, stage_h, frame, crop)
        mirror = mirror_frame_rect(stage_w, stage_h, frame, crop)
        assert isinstance(device, dict), f"{frame} @ {crop}: 设备拒了({device})"
        assert device == mirror, f"{frame} @ {crop}: 设备={device} 镜像={mirror}"
        print(f"  ok  帧 {frame[2]}x{frame[3]} @ ({frame[0]},{frame[1]}) "
              f"落在舞台 ({device['x']},{device['y']})")


def test_wrong_crop_base_is_rejected(exe: str) -> None:
    """减错基准必须被判死: 传屏幕落点(55,38 / 42,38)而不是裁剪原点(31,5 / 18,7)。"""
    wrong = [
        ((129, 198), (31, 5, 129, 198), (55, 38)),
        ((156, 198), (18, 7, 156, 198), (42, 38)),
        # 帧本身就比舞台裁剪框大(包坏了) —— 同样要拒, 别画一个越界的帧。
        ((129, 198), (31, 5, 130, 198), (31, 5)),
        ((129, 198), (31, 5, 129, 199), (31, 5)),
    ]
    for (stage_w, stage_h), frame, crop in wrong:
        device = device_frame_rect(exe, stage_w, stage_h, frame, crop)
        mirror = mirror_frame_rect(stage_w, stage_h, frame, crop)
        assert device == "rejected", (
            f"{frame} @ {crop}: 设备竟然接受了({device}) —— 落点基准又错回屏幕坐标了")
        assert mirror == "rejected", (
            f"{frame} @ {crop}: 镜像接受了不该接受的落点({mirror})")
    print(f"  ok  {len(wrong)} 个减错基准/越界帧两边都拒")


def main() -> int:
    print("预览工具的版式镜像 vs 设备真值")
    with tempfile.TemporaryDirectory(prefix="pet-layout-mirror-") as workdir:
        exe = build_dumper(workdir)
        test_fields_match(exe)
        test_rejections_match(exe)
        test_package_bounds_match(exe)
        test_frame_landing_matches(exe)
        test_wrong_crop_base_is_rejected(exe)
    print("Pet layout mirror: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
