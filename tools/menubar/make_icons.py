#!/usr/bin/env python3
"""从源图 CodexPetBridge.png 派生菜单栏图标与应用图标。

产物（都写在同目录 Assets/ 下）：
  menu-icon.png      18px   菜单栏 1x
  menu-icon@2x.png   36px   菜单栏 2x
  AppIcon.icns       多尺寸 应用图标（Finder 用，CFBundleIconFile=AppIcon）
  about-icon.png     256px  "关于"面板用（真透明，绕开系统图标合成）

macOS 26 (Tahoe) 的图标系统会把透明异形 icns 自动垫进系统灰色 squircle 底板,
应用图标没法真透明。试过 full-bleed 自带底色, 但底色和设备图不协调, 已放弃
(队长拍板: 灰就灰吧); "关于"面板不走系统合成, 用透明的 about-icon.png 真透明。

流程：alpha>8 裁透明边 → 补正方形并留 6% 边距 → 高质量缩放。
源图只留档，不要手改派生产物；换了源图就重跑本脚本：

    <带 Pillow 的 python> tools/menubar/make_icons.py

icns 用 Pillow 的 ICNS 写入（含 16..1024 全尺寸）。
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
SRC = HERE / "CodexPetBridge.png"
OUT_DIR = HERE / "Assets"

ALPHA_THRESHOLD = 8   # 与菜单图标同参数：低于它的像素当隐形
MARGIN = 0.06         # 补方形时四周各留 6%
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]
MENU_SIZES = {"menu-icon.png": 18, "menu-icon@2x.png": 36}
ABOUT_SIZE = 256


def trimmed(src: Image.Image) -> Image.Image:
    """裁掉透明边, 返回只含设备的紧致图。"""
    alpha = src.split()[-1]
    bbox = alpha.point(lambda a: 255 if a > ALPHA_THRESHOLD else 0).getbbox()
    return src.crop(bbox) if bbox else src


def transparent_square(src: Image.Image) -> Image.Image:
    """裁掉透明边后补成正方形（短边用透明像素填充），四周留 6% 边距。
    用于菜单栏图标、应用图标和"关于"面板 —— Tahoe 会给应用图标垫灰板,
    那是系统行为; 其余两处直接画, 透明是真的透明。"""
    src = trimmed(src)
    side = int(round(max(src.size) / (1 - 2 * MARGIN)))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    off = ((side - src.width) // 2, (side - src.height) // 2)
    canvas.paste(src, off, src)
    return canvas


def save_scaled(base: Image.Image, path: Path, size: int) -> None:
    base.resize((size, size), Image.LANCZOS).save(path)


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
    transp.save(icns_path, format="ICNS",
                sizes=[(s, s) for s in ICNS_SIZES])
    print(f"AppIcon.icns: {', '.join(str(s) for s in ICNS_SIZES)}px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
