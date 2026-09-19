#!/usr/bin/env python3
"""把宠物界面渲染成 PNG 预览, 不接真机也能看版式。

为什么要从"生成产物"而不是从源图集画:
  - 帧表从 main/pet_atlas_<pet>_portrait.c 解析, 像素从
    main/assets/pet_<pet>_portrait.bin 读 —— 也就是固件真正烧进去的那份字节。
    所以这个预览同时也在验证生成脚本的裁剪偏移和 blob 布局是不是对的:
    如果偏移算错, 预览里的宠物立刻会缺一块或错位。
  - 布局常量从 main/pet_ui.c 里解析, 不复制一份, 避免两边慢慢跑偏。

文字用系统 CJK 字体近似(设备上用的是子集化的 Noto Sans CJK SC), 所以字形会有
细微差别, 位置和字号是准的。

用法:
    python3 tools/preview_pet_screen.py                       # 每个状态一张
    python3 tools/preview_pet_screen.py --anim working        # 只画指定动作
    python3 tools/preview_pet_screen.py --frame 3             # 画该动作的第 3 帧
    python3 tools/preview_pet_screen.py --battery 95%         # 指定顶栏电量
    python3 tools/preview_pet_screen.py --out-dir /tmp/preview
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageFilter

REPO_ROOT = Path(__file__).resolve().parents[1]

# 与 main/pet_ui.c 的调色板保持一致。颜色是十六进制常量, 正则抓不到, 只能抄 —
# 改了 pet_ui.c 的 COL_* 就要回来一起改。
COLORS = {
    "COL_BG": (0x0B, 0x0F, 0x14),
    "COL_CARD": (0x16, 0x20, 0x2A),
    "COL_CARD_EDGE": (0x22, 0x32, 0x3F),
    "COL_PLAT_FLOOR": (0x22, 0x34, 0x44),
    "COL_PLAT_RIM": (0x3E, 0x63, 0x83),
    "COL_PLAT_BODY": (0x13, 0x1B, 0x24),
    "COL_INK": (0xE8, 0xF1, 0xF5),
    "COL_MUTED": (0x7C, 0x93, 0xA3),
    "COL_ACCENT": (0x4C, 0xC2, 0xFF),
    "COL_GOOD": (0x4A, 0xDE, 0x80),
    "COL_WARN": (0xFB, 0xBF, 0x24),
    "COL_BAD": (0xF8, 0x71, 0x71),
}

# 状态 -> (状态文字, 圆点颜色, 站台占位文案, 动作行, 是否睡眠)
SCENES = [
    ("idle",        "空闲",     "COL_GOOD",  "等待 Codex 任务", "idle",    False),
    ("working",     "工作中",   "COL_ACCENT", "Codex 正在执行", "running", False),
    ("waiting",     "等待确认", "COL_WARN",  "需要你确认",      "waiting", False),
    ("ready",       "已完成",   "COL_GOOD",  "任务已完成",      "review",  False),
    ("failed",      "出错了",   "COL_BAD",   "任务中断",        "failed",  False),
    ("offline",     "离线",     "COL_MUTED", "未连接 Codex",    "idle",    True),
]

# 字体候选: macOS 上优先苹方/冬青黑, Linux 上退到 Noto。
FONT_CANDIDATES = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
]


def parse_layout(pet_ui: Path) -> dict[str, int]:
    """从 main/pet_ui.c 抓布局常量, 避免在这里再抄一份。

    只认整数字面量。pet_ui.c 里的长度/坐标因此都写成字面量, 互相之间的等式
    由那边的 _Static_assert 保证 —— 写成宏算式这边会静默漏掉, 预览就会错位。
    行尾的 // 注释允许存在。
    """
    source = pet_ui.read_text(encoding="utf-8")
    layout: dict[str, int] = {}
    for name, value in re.findall(r"^#define\s+([A-Z_0-9]+)\s+(-?\d+)\s*(?://.*)?$",
                                  source, re.MULTILINE):
        layout[name] = int(value)
    return layout


def parse_font_line_height(font_c: Path) -> int:
    """读 LVGL 生成字体的行高。

    这里以前写死 21, 而设备上是 31: 预览的文本块因此比真机矮了一截, 看版式会
    被误导(真机上两行文本是 62 px, 不是 42)。字体文件是生成产物, 直接读真值。
    """
    match = re.search(r"\.line_height\s*=\s*(\d+)",
                      font_c.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"{font_c} 里找不到 line_height")
    return int(match.group(1))


def parse_atlas(header: Path, source: Path) -> tuple[dict[str, int], list[dict]]:
    """解析生成的头文件常量与 .c 里的帧表。"""
    constants = {}
    for name, value in re.findall(r"^#define\s+(PET_ATLAS_[A-Z_0-9]+)\s+(\d+)\s*$",
                                  header.read_text(encoding="utf-8"), re.MULTILINE):
        constants[name] = int(value)

    # 每个动作一个 C 数组: static const pet_atlas_frame_t NAME_FRAMES[] = {
    #     offset, x, y, w, h, duration }, ...
    frames: list[dict] = []
    for block in re.finditer(
        r"static const pet_atlas_frame_t (\w+)_FRAMES\[\] = \{(.*?)\n\};",
        source.read_text(encoding="utf-8"), re.DOTALL,
    ):
        for numbers in re.findall(r"\{\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
                                  block.group(2)):
            offset, x, y, w, h, duration = (int(n) for n in numbers)
            frames.append({
                "name": block.group(1).lower(),
                "offset": offset, "x": x, "y": y, "w": w, "h": h,
                "duration": duration,
            })
    return constants, frames


def rgb565a8_to_image(blob: bytes, offset: int, w: int, h: int) -> Image.Image:
    """把 blob 里的一帧 RGB565A8 解成 RGBA。

    RGB565A8 = 先 w*h 个 RGB565(小端), 再 w*h 个 alpha 字节。
    """
    stride = w * 2
    color = blob[offset:offset + stride * h]
    alpha = blob[offset + stride * h:offset + stride * h + w * h]
    if len(color) < stride * h or len(alpha) < w * h:
        raise ValueError(f"blob 越界: offset={offset} w={w} h={h}")

    try:
        rgb = Image.frombytes("RGB", (w, h), color, "raw", "BGR;16")
    except ValueError:
        # 有些 Pillow 构建没有 BGR;16 解码器, 手工展开 565。
        pixels = bytearray()
        for i in range(0, len(color), 2):
            value = color[i] | (color[i + 1] << 8)
            r = (value >> 11) & 0x1F
            g = (value >> 5) & 0x3F
            b = value & 0x1F
            pixels += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
        rgb = Image.frombytes("RGB", (w, h), bytes(pixels))

    rgb.putalpha(Image.frombytes("L", (w, h), alpha))
    return rgb


def load_font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return ImageFont.load_default()


def text_width(draw: ImageDraw.ImageDraw, text: str,
               font: ImageFont.FreeTypeFont) -> int:
    box = draw.textbbox((0, 0), text, font=font)
    return box[2] - box[0]


def rounded_mask(width: int, height: int, radius: int) -> Image.Image:
    mask = Image.new("L", (width, height), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, width - 1, height - 1), radius=radius, fill=255)
    return mask


def parse_battery(text: str) -> int | None:
    """把 --battery 的取值转成 0..100 的整数; "--" 这类写法代表未知。"""
    value = text.strip().rstrip("%").strip()
    return max(0, min(100, int(value))) if value.isdigit() else None


def paste_platform(board: Image.Image, layout: dict[str, int]) -> None:
    """画站台: 台身 + 台面(竖直渐变) + 台面顶沿高光 + 脚底接触阴影。

    分层顺序与 main/pet_ui.c 的 build_platform() 一一对应, 而且必须在宠物之前
    调用 —— 真机上宠物压在台面之上, 预览也要一样, 否则脚会被台面盖掉。
    """
    plat_x, plat_w = layout["PLAT_X"], layout["PLAT_W"]

    body = Image.new("RGBA", (plat_w, layout["PLAT_BODY_H"]),
                     COLORS["COL_PLAT_BODY"] + (255,))
    board.paste(body, (plat_x, layout["PLAT_BODY_Y"]),
                rounded_mask(plat_w, layout["PLAT_BODY_H"], layout["PLAT_RADIUS"]))
    ImageDraw.Draw(board).rounded_rectangle(
        (plat_x, layout["PLAT_BODY_Y"],
         plat_x + plat_w - 1, layout["PLAT_BODY_Y"] + layout["PLAT_BODY_H"] - 1),
        radius=layout["PLAT_RADIUS"], outline=COLORS["COL_CARD_EDGE"], width=1)

    # 台面: 台身色到台面色的竖直渐变, 用逐行填充做, 与 LV_GRAD_DIR_VER 等价。
    floor_h = layout["PLAT_FLOOR_H"]
    floor = Image.new("RGBA", (plat_w, floor_h), (0, 0, 0, 0))
    paint = ImageDraw.Draw(floor)
    top, bottom = COLORS["COL_PLAT_FLOOR"], COLORS["COL_PLAT_BODY"]
    for row in range(floor_h):
        t = row / max(floor_h - 1, 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
        paint.line((0, row, plat_w - 1, row), fill=color + (255,))
    board.paste(floor, (plat_x, layout["PLAT_FLOOR_Y"]),
                rounded_mask(plat_w, floor_h, layout["PLAT_FLOOR_RADIUS"]))

    ImageDraw.Draw(board).rounded_rectangle(
        (plat_x + layout["PLAT_RIM_INSET"], layout["PLAT_FLOOR_Y"],
         plat_x + plat_w - 1 - layout["PLAT_RIM_INSET"],
         layout["PLAT_FLOOR_Y"] + layout["PLAT_RIM_H"] - 1),
        radius=layout["PLAT_RIM_H"] // 2, fill=COLORS["COL_PLAT_RIM"])

    shadow = Image.new("RGBA", (layout["SHADOW_W"], layout["SHADOW_H"]), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (0, 0, layout["SHADOW_W"] - 1, layout["SHADOW_H"] - 1),
        radius=layout["SHADOW_H"] // 2, fill=(0, 0, 0, 102))   # LV_OPA_40
    board.paste(shadow, (board.width // 2 - layout["SHADOW_W"] // 2,
                         layout["SHADOW_Y"]), shadow)


def render(scene: tuple, layout: dict[str, int], constants: dict[str, int],
           frames: list[dict], blob: bytes, frame_index: int | None,
           battery_text: str = "82%", body_line_height: int = 31) -> Image.Image:
    key, status_text, dot_color, plate_text, anim, asleep = scene

    board = Image.new("RGB", (240, 320), COLORS["COL_BG"])
    draw = ImageDraw.Draw(board)

    # 宠物身下不再有底色; "地面"由站台和接触阴影交代。
    paste_platform(board, layout)

    stage = Image.new("RGBA", (constants["PET_ATLAS_STAGE_W"],
                               constants["PET_ATLAS_STAGE_H"]), (0, 0, 0, 0))
    candidates = [f for f in frames if f["name"] == anim] or [frames[0]]
    chosen = candidates[(frame_index or 0) % len(candidates)]
    sprite = rgb565a8_to_image(blob, chosen["offset"], chosen["w"], chosen["h"])
    if asleep:
        alpha = sprite.getchannel("A").point(lambda v: int(v * 0.4))
        sprite.putalpha(alpha)
    stage.alpha_composite(
        sprite,
        (chosen["x"] - constants["PET_ATLAS_STAGE_X"],
         chosen["y"] - constants["PET_ATLAS_STAGE_Y"]))
    board.paste(stage, (layout["STAGE_X"], layout["STAGE_Y"]), stage)

    # 状态圆点 + 文字
    draw.ellipse((layout["ROW_DOT_X"], layout["ROW_DOT_Y"],
                  layout["ROW_DOT_X"] + layout["ROW_DOT_D"] - 1,
                  layout["ROW_DOT_Y"] + layout["ROW_DOT_D"] - 1),
                 fill=COLORS[dot_color])
    title_font = load_font(20)
    draw.text((layout["STATUS_X"], layout["STATUS_Y"] - 4), status_text,
              font=title_font, fill=COLORS["COL_INK"])

    # 电量: 文字与填充比例都跟着 --battery 走, 默认 82% 只是示意值。
    # 未读数时固件显示 "--" 且填充不可见, 这里保持一致, 方便和真机对照。
    body_font = load_font(16)
    soc = parse_battery(battery_text)
    battery_color = ("COL_GOOD" if soc is None or soc >= 50
                     else "COL_WARN" if soc >= 20 else "COL_BAD")
    draw.text((layout["BAT_TEXT_X"] + layout["BAT_TEXT_W"]
               - text_width(draw, battery_text, body_font),
               layout["BAT_TEXT_Y"] - 1),
              battery_text, font=body_font,
              fill=COLORS["COL_MUTED" if soc is None else battery_color])
    draw.rounded_rectangle(
        (layout["BAT_BODY_X"], layout["BAT_BODY_Y"],
         layout["BAT_BODY_X"] + layout["BAT_BODY_W"] - 1,
         layout["BAT_BODY_Y"] + layout["BAT_BODY_H"] - 1),
        radius=3, outline=COLORS["COL_MUTED"], width=1)
    draw.rounded_rectangle(
        (layout["BAT_CAP_X"], layout["BAT_CAP_Y"],
         layout["BAT_CAP_X"] + layout["BAT_CAP_W"] - 1,
         layout["BAT_CAP_Y"] + layout["BAT_CAP_H"] - 1),
        radius=1, fill=COLORS["COL_MUTED"])
    if soc is not None:
        fill_w = max(int(layout["BAT_FILL_MAX_W"] * soc / 100), 1)
        draw.rounded_rectangle(
            (layout["BAT_FILL_X"], layout["BAT_FILL_Y"],
             layout["BAT_FILL_X"] + fill_w - 1,
             layout["BAT_FILL_Y"] + layout["BAT_FILL_H"] - 1),
            radius=1, fill=COLORS[battery_color])

    # 睡眠标记
    if asleep:
        draw.text((layout["STAGE_X"] + layout["SLEEP_X"],
                   layout["STAGE_Y"] + layout["SLEEP_Y"]),
                  "Zzz", font=body_font, fill=COLORS["COL_ACCENT"])

    # 文本落在台身上: 真机上是一个 PLAT_TEXT_W x (行高x2) 的居中标签, 预览只画
    # 一行。行高从生成字体里读, 所以字体换了这边也跟着走 —— 以前写死 21, 真机
    # 是 31, 文本块的位置是碰巧对上的。
    line_top = layout["PLAT_BODY_Y"] + layout["PLAT_TEXT_PAD"]
    text_y = line_top + (body_line_height - body_font.size) // 2
    color = COLORS["COL_INK"] if not plate_text.startswith("等待 Codex") else COLORS["COL_MUTED"]
    draw.text((layout["PLAT_X"] + layout["PLAT_W"] // 2
               - text_width(draw, plate_text, body_font) // 2,
               text_y + 2),
              plate_text, font=body_font, fill=color)

    # 与 BSP 一致的 30 px 圆角黑边(BSP_LVGL_SCREEN_RADIUS)
    mask = rounded_mask(240, 320, 30)
    out = Image.new("RGB", (240, 320), (0, 0, 0))
    out.paste(board, (0, 0), mask)
    out.info["scene"] = key
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="渲染宠物界面预览 PNG")
    parser.add_argument("--pet-id", default="sophie-portrait")
    parser.add_argument("--anim", help="只渲染指定动作行(如 working/idle)")
    parser.add_argument("--frame", type=int, default=None, help="指定帧序号")
    parser.add_argument("--out-dir", type=Path,
                        default=REPO_ROOT / "assets" / "pets"
                        / "sophie-portrait" / "preview")
    parser.add_argument("--scale", type=int, default=2, help="输出放大倍数")
    parser.add_argument("--battery", default="82%",
                        help="顶栏电量, 如 95%% 或 -- (表示未读到)")
    args = parser.parse_args()

    slug = args.pet_id.replace("-", "_")
    header = REPO_ROOT / "main" / f"pet_atlas_{slug}.h"
    source = REPO_ROOT / "main" / f"pet_atlas_{slug}.c"
    blob_path = REPO_ROOT / "main" / "assets" / f"pet_{slug}.bin"
    for path in (header, source, blob_path):
        if not path.exists():
            print(f"缺少生成产物 {path}; 先运行 tools/gen_pet_assets.py")
            return 1

    font_c = REPO_ROOT / "assets" / "fonts" / "pet_font_16.c"
    if not font_c.exists():
        print(f"缺少生成产物 {font_c}; 先运行 tools/gen_pet_fonts.py")
        return 1

    layout = parse_layout(REPO_ROOT / "main" / "pet_ui.c")
    constants, frames = parse_atlas(header, source)
    body_line_height = parse_font_line_height(font_c)
    # 床垫(bin)按需读: 2.4 MB, 一次读进来比反复 seek 简单。
    blob = blob_path.read_bytes()
    print(f"帧表 {len(frames)} 帧, blob {len(blob)} 字节, "
          f"舞台 {constants['PET_ATLAS_STAGE_W']}x{constants['PET_ATLAS_STAGE_H']}, "
          f"正文行高 {body_line_height}")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    rendered = 0
    for scene in SCENES:
        if args.anim and scene[4] != args.anim:
            continue
        image = render(scene, layout, constants, frames, blob, args.frame,
                       args.battery, body_line_height)
        if args.scale > 1:
            image = image.resize((image.width * args.scale, image.height * args.scale),
                                 Image.NEAREST)
        out = args.out_dir / f"screen-{scene[0]}.png"
        image.save(out)
        print(f"  -> {out}")
        rendered += 1

    if rendered == 0:
        print(f"没有匹配 --anim={args.anim} 的场景; "
              f"可选: {', '.join(s[4] for s in SCENES)}")
        return 1

    # 再把所有状态拼成一张联系表, 方便一眼比较。
    sheets = [Image.open(args.out_dir / f"screen-{s[0]}.png") for s in SCENES
              if not args.anim or s[4] == args.anim]
    sheet = Image.new("RGB", (sum(i.width for i in sheets) + 12 * (len(sheets) - 1),
                              max(i.height for i in sheets)), (0, 0, 0))
    x = 0
    for image in sheets:
        sheet.paste(image, (x, 0))
        x += image.width + 12
    sheet_path = args.out_dir / "screen-all.png"
    sheet.save(sheet_path)
    print(f"  -> {sheet_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
