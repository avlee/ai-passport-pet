#!/usr/bin/env python3
"""从源图与图标工程派生菜单栏图标与应用图标。

产物（都写在同目录 Assets/ 下）：
  menu-icon.png      18px   菜单栏 1x
  menu-icon@2x.png   36px   菜单栏 2x
  AppIcon.icns       多尺寸 应用图标（Finder 用，CFBundleIconFile=AppIcon）
  about-icon.png     256px  "关于"面板用（真透明，绕开系统图标合成）

两个来源、两条流水线，别混：

1. AppIcon.icns ← AppIcon.icon（Icon Composer 图标工程）。
   ictool 用系统合成器渲出真 Liquid Glass（阴影/景深/半透明都是算出来的，
   底色取工程里的 fill），再缩放成 iconset 交给 iconutil 打成 icns。
   icns 是静态格式，装进应用后深色模式也显示同一张 Default 外观；
   要真正随外观自适应得走 actool 编 Assets.car，那需要完整 Xcode，
   本仓库只用 Command Line Tools 构建，故不做（取舍记录在此）。

2. menu-icon / about-icon ← 源图 CodexPetBridge.png（平面派生）。
   这两处要真透明，而 .icon 渲染产物自带底板，不能替代。

历史注记：Tahoe 曾把透明异形 icns 自动垫进系统灰色 squircle 底板
(队长拍板"灰就灰吧")。AppIcon.icon 引入后应用图标自带设计好的底色，
灰色垫板问题不复存在；"关于"面板仍走真透明，不受影响。

流程（源图）：alpha>8 裁透明边 → 补正方形并留 6% 边距 → 高质量缩放。
派生产物不要手改；换了源图或图标工程就重跑本脚本：

    <带 Pillow 的 python> tools/menubar/make_icons.py

没装 Icon Composer 时 AppIcon.icns 退回旧的 Pillow 平面派生（源图补方形，
含 16..1024 全尺寸），保证脚本在任何机器都能跑完。
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
SRC = HERE / "CodexPetBridge.png"
OUT_DIR = HERE / "Assets"
ICON_DOC = OUT_DIR / "AppIcon.icon"

ALPHA_THRESHOLD = 8   # 与菜单图标同参数：低于它的像素当隐形
MARGIN = 0.06         # 补方形时四周各留 6%
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]
MENU_SIZES = {"menu-icon.png": 18, "menu-icon@2x.png": 36}
ABOUT_SIZE = 256
RENDER_SIZE = 1024    # ictool 渲染边长，iconset 里最大的 512@2x 正好吃满

# ictool 随 Icon Composer 安装；独立安装在前，Xcode 内置在后。
ICTOOL_CANDIDATES = [
    Path("/Applications/Icon Composer.app/Contents/Executables/ictool"),
    Path("/Applications/Xcode.app/Contents/Applications/Icon Composer.app"
         "/Contents/Executables/ictool"),
]


def find_ictool() -> Path | None:
    for candidate in ICTOOL_CANDIDATES:
        if candidate.exists():
            return candidate
    return None


def trimmed(src: Image.Image) -> Image.Image:
    """裁掉透明边, 返回只含设备的紧致图。"""
    alpha = src.split()[-1]
    bbox = alpha.point(lambda a: 255 if a > ALPHA_THRESHOLD else 0).getbbox()
    return src.crop(bbox) if bbox else src


def transparent_square(src: Image.Image) -> Image.Image:
    """裁掉透明边后补成正方形（短边用透明像素填充），四周留 6% 边距。
    用于菜单栏图标和"关于"面板 —— 透明是真的透明。"""
    src = trimmed(src)
    side = int(round(max(src.size) / (1 - 2 * MARGIN)))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    off = ((side - src.width) // 2, (side - src.height) // 2)
    canvas.paste(src, off, src)
    return canvas


def save_scaled(base: Image.Image, path: Path, size: int) -> None:
    base.resize((size, size), Image.LANCZOS).save(path)


def icns_from_icon_document(ictool: Path, out: Path) -> None:
    """AppIcon.icon → ictool 渲染 → iconset → iconutil → AppIcon.icns。"""
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        render = work / "default.png"
        subprocess.run(
            [str(ictool), str(ICON_DOC), "--export-image",
             "--output-file", str(render), "--platform", "macOS",
             "--rendition", "Default",
             "--width", str(RENDER_SIZE), "--height", str(RENDER_SIZE),
             "--scale", "1"],
            check=True, capture_output=True)
        composed = Image.open(render).convert("RGBA")

        iconset = work / "AppIcon.iconset"
        iconset.mkdir()
        # 标准十件套：每档 1x/2x，最大 512@2x=1024，正好是渲染边长。
        for s in (16, 32, 128, 256, 512):
            for name, px in ((f"icon_{s}x{s}.png", s),
                             (f"icon_{s}x{s}@2x.png", s * 2)):
                composed.resize((px, px), Image.LANCZOS).save(iconset / name)
        subprocess.run(["iconutil", "-c", "icns", str(iconset),
                        "-o", str(out)], check=True, capture_output=True)


def icns_flat_fallback(transp: Image.Image, out: Path) -> None:
    """没有 ictool 时的退路：源图补方形后用 Pillow 直写 icns（平面图，
    无 Liquid Glass）。产物保底可用，但不带新图标的底色和质感。"""
    transp.save(out, format="ICNS",
                sizes=[(s, s) for s in ICNS_SIZES])


def main() -> int:
    src = Image.open(SRC).convert("RGBA")
    transp = transparent_square(src)
    try:
        OUT_DIR.mkdir(exist_ok=True)
    except FileExistsError:
        pass
    except PermissionError:
        # 沙箱/受限环境有时把 exist_ok 的吞错也拦掉; 目录已在就不影响产出。
        if not OUT_DIR.is_dir():
            raise

    for name, size in MENU_SIZES.items():
        save_scaled(transp, OUT_DIR / name, size)
        print(f"{name}: {size}px")

    save_scaled(transp, OUT_DIR / "about-icon.png", ABOUT_SIZE)
    print(f"about-icon.png: {ABOUT_SIZE}px (透明)")

    icns_path = OUT_DIR / "AppIcon.icns"
    ictool = find_ictool()
    if ictool is not None:
        icns_from_icon_document(ictool, icns_path)
        print(f"AppIcon.icns: 来自 {ICON_DOC.name} (ictool Liquid Glass, "
              f"Default 外观)")
    else:
        if ICON_DOC.exists():
            print("提示: 没找到 ictool (装 Icon Composer 后重跑可得 "
                  "Liquid Glass 版图标), AppIcon.icns 退回平面派生。")
        icns_flat_fallback(transp, icns_path)
        print(f"AppIcon.icns: 平面派生, "
              f"{', '.join(str(s) for s in ICNS_SIZES)}px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
