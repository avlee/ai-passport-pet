#!/usr/bin/env python3
"""把宠物界面渲染成预览 PNG, 不接真机也能看版式。

编辑器里看不到那块 240x320 的屏, 所以预览是唯一能"眼见为实"的途径。为了让它真的
可信, 这里不复制任何一份数据, 全部从源头读:

  * 像素与帧表来自一份 **.pet 包**(默认用 tools/gen_pet_package.py 现打一份)。
    用包而不是源图集, 是因为要看的正是设备会收到的那串字节 —— 打包时的裁剪偏移
    算错, 预览里立刻缺一块或错位。``--package`` 还可以直接指一个现成的 .pet,
    用来确认"设备上那份到底是什么样"。
  * 布局常量从 main/pet_layout.h/.c 与 main/pet_ui.c 的 #define 解析;
    配色从 pet_ui.c 的 COL_* 解析; 文案从 main/pet_strings.h 解析。
  * 字体行高与基线从 assets/fonts/pet_font_{16,20}.c 读。

文字用系统 CJK 字体近似(设备上是子集化的 Noto Sans CJK SC), 所以字形有细微差别;
**位置是按 LVGL 的算法算的**, 不是估的:

    基线 = 行框顶 + line_height - base_line

(见 lvgl 的 lv_draw_label.c:626; base_line 的口径是"从行框底边量起", 见生成字体里
的注释。)行高与基线都取自生成字体, 所以换字体/换字号这边会自动跟上。

用法:
    python3 tools/preview_pet_screen.py                     # 全部屏, 每屏一张
    python3 tools/preview_pet_screen.py --pet li-muwan      # 换一只宠物出图
    python3 tools/preview_pet_screen.py --package out.pet   # 用现成的包
    python3 tools/preview_pet_screen.py --anim working      # 只画指定动作的屏
    python3 tools/preview_pet_screen.py --frame 3           # 画该动作的第 3 帧
    python3 tools/preview_pet_screen.py --battery 95%       # 指定顶栏电量
    python3 tools/preview_pet_screen.py --plan pro          # 指定订阅徽标档位
    python3 tools/preview_pet_screen.py --out-dir /tmp/preview
"""

from __future__ import annotations

import argparse
import math
import os
import re
import sys
from pathlib import Path

# 出图当然要 Pillow, 但 tests/test_pet_layout_mirror.py 只借走下面的版式算术
# (它要在没有 Pillow 的解释器上也能跑), 所以这里不强求。
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:   # pragma: no cover - 只在没装 Pillow 的解释器上走到
    Image = ImageDraw = ImageFont = None   # type: ignore[assignment]

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import gen_pet_package as gen  # noqa: E402  (路径在上面设好了)

LAYOUT_H = REPO_ROOT / "main" / "pet_layout.h"
LAYOUT_C = REPO_ROOT / "main" / "pet_layout.c"
UI_C = REPO_ROOT / "main" / "pet_ui.c"
STRINGS_H = REPO_ROOT / "main" / "pet_strings.h"
FONT_C = {16: REPO_ROOT / "assets" / "fonts" / "pet_font_16.c",
          20: REPO_ROOT / "assets" / "fonts" / "pet_font_20.c"}
# LVGL 内置 montserrat 的 (行高, 基线) —— 宠物界面只用 12 一档(订阅徽标 + 能量槽
# 小标签各一处), 它没有对应的生成字体文件, 度量取 LVGL 源码常量
# (managed_components/lvgl__lvgl/src/font/lv_font_montserrat_12.c):
#   .line_height = 15, .base_line = 3
# base_line 是"从行框底边往上量"/*Baseline measured from the bottom of the line*/;
# 这一档曾错写成 (16, 11) —— 把 base_line 当成"从顶边量起", 文字整体被顶出框外。
# 这类手写常量最容易跟源码脱钩, 改动前先去那份源码里核一眼。
M12_METRICS = (15, 3)

# 布局常量分散在三个文件里: 屏幕尺寸与舞台上下限在 pet_layout.h, 站台坐标在
# pet_layout.c(那边有主机测试), 顶栏/配网页/传输页与宠物无关, 在 pet_ui.c。
# 三处都必须写成整数字面量 —— 宏算式这里读不懂、会被静默漏掉。
CONSTANT_SOURCES = (LAYOUT_H, LAYOUT_C, UI_C)

# 状态屏。(文件名后缀, 状态文案键, 圆点颜色, 站台文案键, 动作, 是否睡眠)
# 动作名必须与 .pet 的状态表一致(main/pet_state.h 的 pet_anim_t)。
SCENES = [
    ("idle",    "PET_STR_STATUS_IDLE",    "COL_GOOD",   "PET_STR_PH_IDLE",    "idle",    False),
    ("working", "PET_STR_STATUS_WORKING", "COL_ACCENT", "PET_STR_PH_WORKING", "running", False),
    ("waiting", "PET_STR_STATUS_WAITING", "COL_WARN",   "PET_STR_PH_WAITING", "waiting", False),
    ("ready",   "PET_STR_STATUS_READY",   "COL_GOOD",   "PET_STR_PH_READY",   "review",  False),
    ("failed",  "PET_STR_STATUS_FAILED",  "COL_BAD",    "PET_STR_PH_FAILED",  "failed",  False),
    ("offline", "PET_STR_STATUS_OFFLINE", "COL_MUTED",  "PET_STR_PH_OFFLINE", "idle",    True),
]

# 与宠物无关的三块屏: 空槽 / 传输页 / 配网页。
STATIC_SCREENS = ("nopet", "transfer", "prov")

# 设备上的字体是子集化的 Noto Sans CJK SC; 这里只求"字形像", 位置另算。
# 注意 PingFang.ttc 在较新的 macOS 上已经不在这个路径, 且 FreeType 也打不开它 ——
# 排在后面当兜底即可。
FONT_CANDIDATES = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
]


# ---------------------------------------------------------------------------
# 从源码里读真值
# ---------------------------------------------------------------------------
def parse_defines(paths: tuple[Path, ...]) -> dict[str, int]:
    """解析 #define 的整数字面量(十进制或 0x 十六进制), 多个来源合并。

    只认字面量: 布局常量写成宏算式这边读不到, 所以那份约定(见 main/pet_layout.h)
    必须守住, 常量之间的等式由那边的 _Static_assert 在编译期兜住。
    同一个名字出现在多个文件里且值不一致时直接报错 —— 那说明两份真值跑偏了。
    """
    merged: dict[str, int] = {}
    origin: dict[str, str] = {}
    for path in paths:
        pattern = (r"^#define\s+([A-Z][A-Z_0-9]*)\s+"
                   r"(0[xX][0-9A-Fa-f]+|-?\d+)\s*(?://.*)?$")
        for name, raw in re.findall(pattern, path.read_text(encoding="utf-8"),
                                    re.MULTILINE):
            value = int(raw, 0)
            if name in merged and merged[name] != value:
                raise SystemExit(f"常量 {name} 在 {origin[name]} 与 {path.name} 里"
                                 f"不一致: {merged[name]} vs {value}")
            merged[name] = value
            origin[name] = path.name
    return merged


def parse_strings(path: Path) -> dict[str, str]:
    """界面文案。设备上显示什么, 预览就画什么 —— 不在这里再抄一遍中文。"""
    return dict(re.findall(r'^#define\s+(PET_STR_[A-Z_0-9]+)\s+"([^"]*)"\s*$',
                           path.read_text(encoding="utf-8"), re.MULTILINE))


def parse_font_metrics(font_c: Path) -> tuple[int, int]:
    """生成字体的 (行高, 基线)。见文件头对基线的说明。"""
    source = font_c.read_text(encoding="utf-8")
    line_height = re.search(r"\.line_height\s*=\s*(\d+)", source)
    base_line = re.search(r"\.base_line\s*=\s*(\d+)", source)
    if line_height is None or base_line is None:
        raise SystemExit(f"{font_c} 里找不到 line_height / base_line")
    return int(line_height.group(1)), int(base_line.group(1))


def rgb(value: int) -> tuple[int, int, int]:
    return ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF)


def layout_for_stage(stage_w: int, stage_h: int,
                     consts: dict[str, int]) -> dict[str, int]:
    """按舞台尺寸算版式 —— main/pet_layout.c 的 pet_layout_for_stage() 的镜像。

    真值在那边的 C 代码里(tests/test_pet_layout.c 逐条钉住), 这里是同一套算术的
    第二份实现。算错的表现是"预览看着居中、真机偏几像素", 两边都不报错, 所以由
    tests/test_pet_layout_mirror.py 逐字段对照, 别让两份实现悄悄跑偏。

    水平居中, 竖直方向**按脚底对齐台面反推** —— 所以换一只不同高度的宠物时, 站台
    不动, 是宠物自己站上去。
    """
    if not (consts["PET_LAYOUT_STAGE_W_MIN"] <= stage_w
            <= consts["PET_LAYOUT_STAGE_W_MAX"]):
        raise SystemExit(f"舞台宽 {stage_w} 超出 "
                         f"{consts['PET_LAYOUT_STAGE_W_MIN']}.."
                         f"{consts['PET_LAYOUT_STAGE_W_MAX']}(见 main/pet_layout.h)")
    if stage_h < consts["PET_LAYOUT_STAGE_H_MIN"]:
        raise SystemExit(f"舞台高 {stage_h} 小于下限 {consts['PET_LAYOUT_STAGE_H_MIN']}")

    stage_x = (consts["PET_LAYOUT_SCR_W"] - stage_w) // 2
    stage_y = consts["PET_LAYOUT_PLAT_FLOOR_Y"] + 1 - stage_h
    if stage_y < consts["PET_LAYOUT_STAGE_TOP_MIN"]:
        raise SystemExit(f"舞台 {stage_w}x{stage_h} 顶边会压到顶部状态行"
                         f"(y {stage_y} < {consts['PET_LAYOUT_STAGE_TOP_MIN']})")

    return {
        "stage_x": stage_x, "stage_y": stage_y,
        "stage_w": stage_w, "stage_h": stage_h,

        "plat_x": consts["PLAT_X"], "plat_w": consts["PLAT_W"],
        "plat_radius": consts["PLAT_RADIUS"],
        "plat_floor_y": consts["PET_LAYOUT_PLAT_FLOOR_Y"],
        "plat_floor_h": consts["PET_LAYOUT_PLAT_FLOOR_H"],
        "plat_floor_radius": consts["PLAT_FLOOR_RADIUS"],
        "plat_rim_h": consts["PLAT_RIM_H"],
        "plat_rim_inset": consts["PLAT_RIM_INSET"],
        "plat_body_y": consts["PLAT_BODY_Y"],
        "plat_body_h": consts["PLAT_BODY_H"],
        "plat_text_x": consts["PLAT_X"]
                        + (consts["PLAT_W"] - consts["PLAT_TEXT_W"]) // 2,
        "plat_text_y": consts["PLAT_BODY_Y"] + consts["PLAT_TEXT_PAD"],
        "plat_text_w": consts["PLAT_TEXT_W"],

        "shadow_y": consts["SHADOW_Y"], "shadow_w": consts["SHADOW_W"],
        "shadow_h": consts["SHADOW_H"],

        "sleep_x": consts["SLEEP_X"], "sleep_y": consts["SLEEP_Y"],
    }


def frame_rect(frame: dict, crop: tuple[int, int, int, int]) -> dict[str, int]:
    """一帧在舞台里的落点 —— main/pet_layout.c 的 pet_layout_frame_rect() 的镜像。

    crop 是包头的 (stage_x, stage_y, stage_w, stage_h), 即**所有帧裁剪框的并集**,
    也是每帧裁剪原点要减掉的基准。别拿版式里的 stage_x/stage_y 来代入这个参数:
    那是舞台在**屏幕上**的落点, 在另一个坐标系里, 减错了宠物会整体左上偏移并被
    舞台裁掉一角(设备侧真机上错过一次, 本函数存在的理由就是让两边能对得上)。

    帧的裁剪框必须含在舞台裁剪框里 —— pet_pkg_parse() 保证舞台就是所有帧裁剪框的
    并集, 所以对合法的包这一定成立; 不成立说明包坏了, 宁可报错也不要贴出错位的帧。
    """
    x0, y0, w0, h0 = crop
    if not (x0 <= frame["x"] and y0 <= frame["y"]
            and frame["x"] + frame["w"] <= x0 + w0
            and frame["y"] + frame["h"] <= y0 + h0):
        raise SystemExit(
            f"帧裁剪框 {frame['w']}x{frame['h']} @ ({frame['x']},{frame['y']}) "
            f"超出舞台裁剪框 {w0}x{h0} @ ({x0},{y0}) —— 包与版式对不上")

    return {"x": frame["x"] - x0, "y": frame["y"] - y0,
            "w": frame["w"], "h": frame["h"]}


# ---------------------------------------------------------------------------
# 文字
# ---------------------------------------------------------------------------
def load_font(size: int):
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    print("警告: 找不到可用的 CJK 字体, 退回 Pillow 内置位图字体(汉字会变成方块)",
          file=sys.stderr)
    return ImageFont.load_default()


def wrap_text(draw: ImageDraw.ImageDraw, text: str, font, max_w: float) -> list[str]:
    """按字符折行。中文没有词边界, 所以逐字累计到超宽为止。

    只用来"看起来对", 精确宽度由 tests/test_pet_ui_text_fit.py 按 LVGL 的
    逐字形取整算法负责 —— 那边才是判定文案会不会被折行的地方。
    """
    lines: list[str] = []
    current = ""
    for ch in text:
        if current and draw.textlength(current + ch, font=font) > max_w:
            lines.append(current)
            current = ch
        else:
            current += ch
    if current:
        lines.append(current)
    return lines


def draw_line(draw: ImageDraw.ImageDraw, text: str, font, metrics: tuple[int, int],
              x: float, top_y: float, color, align: str = "left",
              box_w: float = 0.0) -> None:
    """画一行, 基线按 LVGL 的算法落位(见文件头)。top_y 是行框顶。"""
    line_height, base_line = metrics
    baseline = top_y + line_height - base_line
    if align != "left":
        width = draw.textlength(text, font=font)
        x = x + (box_w - width if align == "right" else (box_w - width) / 2)
    draw.text((x, baseline), text, font=font, fill=color, anchor="ls")


def draw_block(draw: ImageDraw.ImageDraw, text: str, font,
               metrics: tuple[int, int], box_x: float, box_w: float,
               top_y: float, color, align: str = "center",
               line_space: int = 0) -> int:
    """在固定宽度的框里画一段(可能折行的)文字, 返回行数。

    LVGL 的定宽标签把第一行贴在内容区顶边, 行与行之间加 line_space(可为负,
    与设备侧 pet_ui.c 的 TEXT_LINE_SPACE 同源), 所以第 k 行的行框顶 =
    top_y + k * (line_height + line_space)。首行不受行距影响。
    """
    line_height = metrics[0]
    step = line_height + line_space
    lines = wrap_text(draw, text, font, box_w)
    for index, line in enumerate(lines):
        draw_line(draw, line, font, metrics, box_x,
                  top_y + index * step, color, align, box_w)
    return len(lines)


# ---------------------------------------------------------------------------
# 图形
# ---------------------------------------------------------------------------
def rgb565a8_to_image(blob: bytes, offset: int, w: int, h: int) -> Image.Image:
    """把 blob 里的一帧 RGB565A8 解成 RGBA。

    RGB565A8 = 先 w*h 个 RGB565(小端), 再 w*h 个 alpha 字节。
    """
    stride = w * 2
    color = blob[offset:offset + stride * h]
    alpha = blob[offset + stride * h:offset + stride * h + w * h]
    if len(color) < stride * h or len(alpha) < w * h:
        raise SystemExit(f"包里的帧数据越界: offset={offset} w={w} h={h}")

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


def rounded_mask(width: int, height: int, radius: int) -> Image.Image:
    mask = Image.new("L", (width, height), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, width - 1, height - 1), radius=radius, fill=255)
    return mask


def paste_platform(board: Image.Image, layout: dict[str, int],
                   colors: dict[str, tuple[int, int, int]]) -> None:
    """画站台: 台身 + 台面(竖直渐变) + 台面顶沿高光 + 脚底接触阴影。

    分层顺序与 main/pet_ui.c 的 build_platform() 一一对应, 而且必须在宠物之前
    调用 —— 真机上宠物压在台面之上, 预览也要一样, 否则脚会被台面盖掉。
    """
    plat_x, plat_w = layout["plat_x"], layout["plat_w"]

    body = Image.new("RGBA", (plat_w, layout["plat_body_h"]),
                     colors["COL_PLAT_BODY"] + (255,))
    board.paste(body, (plat_x, layout["plat_body_y"]),
                rounded_mask(plat_w, layout["plat_body_h"], layout["plat_radius"]))
    ImageDraw.Draw(board).rounded_rectangle(
        (plat_x, layout["plat_body_y"],
         plat_x + plat_w - 1, layout["plat_body_y"] + layout["plat_body_h"] - 1),
        radius=layout["plat_radius"], outline=colors["COL_CARD_EDGE"], width=1)

    # 台面: 台身色到台面色的竖直渐变, 用逐行填充做, 与 LV_GRAD_DIR_VER 等价。
    floor_h = layout["plat_floor_h"]
    floor = Image.new("RGBA", (plat_w, floor_h), (0, 0, 0, 0))
    paint = ImageDraw.Draw(floor)
    top, bottom = colors["COL_PLAT_FLOOR"], colors["COL_PLAT_BODY"]
    for row in range(floor_h):
        t = row / max(floor_h - 1, 1)
        paint.line((0, row, plat_w - 1, row),
                   fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
                   + (255,))
    board.paste(floor, (plat_x, layout["plat_floor_y"]),
                rounded_mask(plat_w, floor_h, layout["plat_floor_radius"]))

    ImageDraw.Draw(board).rounded_rectangle(
        (plat_x + layout["plat_rim_inset"], layout["plat_floor_y"],
         plat_x + plat_w - 1 - layout["plat_rim_inset"],
         layout["plat_floor_y"] + layout["plat_rim_h"] - 1),
        radius=layout["plat_rim_h"] // 2, fill=colors["COL_PLAT_RIM"])

    shadow = Image.new("RGBA", (layout["shadow_w"], layout["shadow_h"]), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (0, 0, layout["shadow_w"] - 1, layout["shadow_h"] - 1),
        radius=layout["shadow_h"] // 2, fill=(0, 0, 0, 102))   # LV_OPA_40
    board.paste(shadow, (board.width // 2 - layout["shadow_w"] // 2,
                         layout["shadow_y"]), shadow)


def parse_battery(text: str) -> int | None:
    """把 --battery 的取值转成 0..100 的整数; "--" 这类写法代表未知。"""
    value = text.strip().rstrip("%").strip()
    return max(0, min(100, int(value))) if value.isdigit() else None


def parse_limits(text: str) -> tuple[int | None, int | None]:
    """--limits 的取值: "<5h窗已用>/<周窗已用>", 如 98/31; 每格用 -- 表示未读到。"""
    parts = text.split("/")
    if len(parts) != 2:
        raise SystemExit(f"--limits 的格式是 <5h已用>/<周已用>, 收到 {text!r}")
    return parse_battery(parts[0]), parse_battery(parts[1])


def draw_top_row(board: Image.Image, draw: ImageDraw.ImageDraw, c: dict[str, int],
                 colors: dict[str, tuple[int, int, int]],
                 status: str, dot: str, battery_text: str,
                 fonts: dict[int, object],
                 metrics: dict[int, tuple[int, int]]) -> None:
    """顶栏: 左 = 状态圆点 + 状态文字, 右 = 电量(数字 + 电池图形)。"""
    draw.ellipse((c["ROW_DOT_X"], c["ROW_DOT_Y"],
                  c["ROW_DOT_X"] + c["ROW_DOT_D"] - 1,
                  c["ROW_DOT_Y"] + c["ROW_DOT_D"] - 1), fill=colors[dot])
    # 状态文字走 title 档(20 px/行高 29), 比右侧电量数字大一档是有意的: 顶栏左半边是
    # 主角。竖直对齐靠 STATUS_Y 让**墨迹中心**与电池图形(y=20)同轴, 不靠行框等高 ——
    # 两侧字号不同, 行框永远等不了高。改任一侧的字号都要重算 STATUS_Y/BAT_TEXT_Y。
    draw_line(draw, status, fonts[20], metrics[20], c["STATUS_X"], c["STATUS_Y"],
              colors["COL_INK"])

    # 电量: 文字与填充比例都跟着 --battery 走, 默认 82% 只是示意值。
    # 未读数时固件显示 "--" 且填充不可见, 这里保持一致, 方便和真机对照。
    soc = parse_battery(battery_text)
    battery_color = ("COL_GOOD" if soc is None or soc >= 50
                     else "COL_WARN" if soc >= 20 else "COL_BAD")
    draw_line(draw, battery_text, fonts[16], metrics[16], c["BAT_TEXT_X"],
              c["BAT_TEXT_Y"], colors["COL_MUTED" if soc is None else battery_color],
              align="right", box_w=c["BAT_TEXT_W"])

    draw.rounded_rectangle(
        (c["BAT_BODY_X"], c["BAT_BODY_Y"],
         c["BAT_BODY_X"] + c["BAT_BODY_W"] - 1,
         c["BAT_BODY_Y"] + c["BAT_BODY_H"] - 1),
        radius=3, outline=colors["COL_MUTED"], width=1)
    draw.rounded_rectangle(
        (c["BAT_CAP_X"], c["BAT_CAP_Y"],
         c["BAT_CAP_X"] + c["BAT_CAP_W"] - 1,
         c["BAT_CAP_Y"] + c["BAT_CAP_H"] - 1),
        radius=1, fill=colors["COL_MUTED"])
    if soc is not None:
        width = max(int(c["BAT_FILL_MAX_W"] * soc / 100), 1)
        draw.rounded_rectangle(
            (c["BAT_FILL_X"], c["BAT_FILL_Y"],
             c["BAT_FILL_X"] + width - 1,
             c["BAT_FILL_Y"] + c["BAT_FILL_H"] - 1),
            radius=1, fill=colors[battery_color])


def plan_label(plan: str | None) -> str | None:
    """订阅档位的展示名 —— main/pet_protocol.c 的 pet_plan_label() 的 Python 镜像。

    规则(与菜单栏 tools/menubar/Sources/BridgeSupervisor.swift 的 codexPlanLabel
    同一条): 去掉首尾空白, 首字母大写, 其余原样。非可打印 ASCII 丢掉 —— 徽标用的
    是内建 Montserrat 12, 只有拉丁字形, 画不出来只会变成缺字方框。

    整不出可显示的内容时返回 None, 调用方据此整块藏起徽标。真值在 C 那边(由
    tests/test_pet_protocol.c 钉住), 这里是同一套算术的第二份实现, 别让两边跑偏。
    """
    if not plan:
        return None
    text = "".join(ch for ch in plan if 0x20 <= ord(ch) <= 0x7E).strip()
    if not text:
        return None
    return text[:1].upper() + text[1:]


def draw_plan_badge(draw: ImageDraw.ImageDraw, c: dict[str, int],
                    colors: dict[str, tuple[int, int, int]],
                    fonts: dict[int, object], layout: dict[str, int],
                    plan: str | None) -> None:
    """顶栏下方的订阅徽标。位置与宽度跟固件的 refresh_plan_locked() 逐式对应:
    右对齐到 PLAN_BADGE_RIGHT, 宽度按文字自适应但避开舞台右边缘 —— 舞台水平居中
    而宽度随宠物变, 越过去就会压在宠物身上。

    填充取"半透明绿叠在背景色上"的**预混合**值: 徽标刻意落在舞台右侧那块空白
    上, 底下必然是背景色, 所以预混合与真机上的 alpha 合成是同一个结果。
    """
    text = plan_label(plan)
    if text is None:
        return   # 没读到档位 / 整不出可显示的字符: 不画, 与固件把胶囊藏起来一致

    avail = c["PLAN_BADGE_RIGHT"] - (layout["stage_x"] + layout["stage_w"])
    if avail < c["PLAN_BADGE_MIN_W"]:
        return   # 这只宠物太宽, 右上角没地方了 —— 固件同样整块藏起来

    width = min(round(draw.textlength(text, font=fonts[12]))
                + 2 * c["PLAN_BADGE_PAD"], avail)
    x = c["PLAN_BADGE_RIGHT"] - width
    top = c["PLAN_BADGE_Y"]
    height = c["PLAN_BADGE_H"]

    green = colors["COL_GOOD"]
    # LV_OPA_30 = 76/255, 取 0.3 就够画图用。
    fill = tuple(round(bg + (g - bg) * 0.3)
                 for bg, g in zip(colors["COL_BG"], green))
    draw.rounded_rectangle((x, top, x + width - 1, top + height - 1),
                           radius=height // 2, fill=fill,
                           outline=green, width=1)
    draw_line(draw, text, fonts[12], M12_METRICS,
              x + c["PLAN_BADGE_PAD"],
              top + (height - M12_METRICS[0]) // 2,
              green, align="center",
              box_w=width - 2 * c["PLAN_BADGE_PAD"])


def finish(board: Image.Image, key: str) -> Image.Image:
    """与 BSP 一致的 30 px 圆角黑边(BSP_LVGL_SCREEN_RADIUS)。"""
    out = Image.new("RGB", (board.width, board.height), (0, 0, 0))
    out.paste(board, (0, 0), rounded_mask(board.width, board.height, 30))
    out.info["screen"] = key
    return out


def draw_gauges(draw: ImageDraw.ImageDraw, layout: dict[str, int],
                c: dict[str, int], colors: dict[str, tuple[int, int, int]],
                strings: dict[str, str], fonts: dict[int, object],
                metrics: dict[int, tuple[int, int]],
                limits: tuple[int | None, int | None]) -> None:
    """站台台身(文字框)内左右两条竖形能量槽。语义与 pet_ui.c 的
    refresh_gauges_locked() 一致: 坐标锚定镜像 layout 的台身几何, 轨道标出
    "满格"高度, 填充按**剩余**百分比从槽底往上生长, 颜色固定(左 = 5 小时窗红,
    右 = 周窗蓝), 槽底一行小标签("5h"/"7d"), 没有快照整组不画。"""
    track_h = (layout["plat_body_h"] - c["GAUGE_VPAD"]
               - c["GAUGE_LABEL_H"] - c["GAUGE_LABEL_GAP"]
               - c["GAUGE_BOTTOM_CLEAR"])
    track_top = layout["plat_body_y"] + c["GAUGE_VPAD"]
    track_x = (layout["plat_x"] + c["GAUGE_INSET"],
               layout["plat_x"] + layout["plat_w"] - c["GAUGE_INSET"] - c["GAUGE_W"])
    labels = (strings.get("PET_STR_LIMIT_PRIMARY", "5h"),
              strings.get("PET_STR_LIMIT_WEEKLY", "7d"))
    bar_colors = ("COL_BAD", "COL_ACCENT")

    for i, used in enumerate(limits):
        if used is None or track_h < c["GAUGE_MIN_H"]:
            continue
        draw.rounded_rectangle(
            (track_x[i], track_top,
             track_x[i] + c["GAUGE_W"] - 1, track_top + track_h - 1),
            radius=2, fill=colors["COL_CARD"])

        remain = 100 - used
        height = max(remain * track_h // 100, c["GAUGE_MIN_H"])
        draw.rounded_rectangle(
            (track_x[i], track_top + track_h - height,
             track_x[i] + c["GAUGE_W"] - 1, track_top + track_h - 1),
            radius=2, fill=colors[bar_colors[i]])

        # 小标签钉在槽底正下方: 左标签左对齐、右标签右对齐于各自的槽。
        # 用的 montserrat_12, 度量取内置常量(见文件头 M12_METRICS)。
        label_x = (track_x[0] if i == 0
                   else track_x[1] + c["GAUGE_W"] - c["GAUGE_LABEL_W"])
        draw_line(draw, labels[i], fonts[12], M12_METRICS, label_x,
                  track_top + track_h + c["GAUGE_LABEL_GAP"],
                  colors["COL_MUTED"])


# ---------------------------------------------------------------------------
# 三块屏
# ---------------------------------------------------------------------------
class Renderer:
    """把常量/字体/文案凑在一起, 免得每个渲染函数都拖着七个参数。"""

    def __init__(self, consts: dict[str, int], strings: dict[str, str],
                 metrics: dict[int, tuple[int, int]]) -> None:
        self.c = consts
        self.s = strings
        self.metrics = metrics
        self.colors = {name: rgb(value) for name, value in consts.items()
                       if name.startswith("COL_")}
        # 只有 12/16/20: 12 = 内置 montserrat(徽标与能量槽标签), 16/20 = 自备中文字库。
        # 14 档已从界面上退场, 不加载。
        self.fonts = {size: load_font(size) for size in (12, 16, 20)}

    def new_board(self) -> tuple[Image.Image, ImageDraw.ImageDraw]:
        size = (self.c["PET_LAYOUT_SCR_W"], self.c["PET_LAYOUT_SCR_H"])
        board = Image.new("RGB", size, self.colors["COL_BG"])
        return board, ImageDraw.Draw(board)

    def block(self, draw: ImageDraw.ImageDraw, text: str, font,
              metrics: tuple[int, int], box_x: float, box_w: float,
              top_y: float, color, align: str = "center") -> int:
        """draw_block 的 Renderer 入口: 行距与设备侧 make_label 同源
        (pet_ui.c 的 TEXT_LINE_SPACE, parse_defines 读的, 缺了直接 KeyError,
        两边跑偏立刻炸)。"""
        return draw_block(draw, text, font, metrics, box_x, box_w, top_y,
                          color, align, self.c["TEXT_LINE_SPACE"])

    # ---- 宠物界面 -----------------------------------------------------
    def pet_screen(self, scene: tuple, package: dict, frame_index: int | None,
                   battery_text: str,
                   limits: tuple[int | None, int | None] = (None, None),
                   plan: str | None = None) -> Image.Image:
        key, status_key, dot, plate_key, anim, asleep = scene
        board, draw = self.new_board()
        layout = layout_for_stage(package["stage"][2], package["stage"][3], self.c)
        paste_platform(board, layout, self.colors)

        # 动作 -> 帧区间由包里的状态表给(与设备侧 pet_atlas_frame() 同源)。
        state = next((s for s in package["states"] if s["name"] == anim), None)
        if state is None:
            raise SystemExit(f"包里没有动作 {anim!r}; "
                             f"可选: {', '.join(s['name'] for s in package['states'])}")
        chosen = package["frames"][state["first"]
                                   + (frame_index or 0) % state["count"]]

        sprite = rgb565a8_to_image(package["blob"], chosen["offset"],
                                  chosen["w"], chosen["h"])
        if asleep:
            sprite.putalpha(sprite.getchannel("A").point(lambda v: int(v * 0.4)))

        # 每帧在图集单元格里的裁剪原点不同, 扣掉**舞台在图集单元格里的裁剪原点**
        # (包头的 stage_x/stage_y, 也就是所有帧裁剪框的并集原点)才是它在舞台里的
        # 落点 —— 动作里的位移(跑动、跳跃)就是这么来的。设备侧同一条式子, 由
        # pet_layout_frame_rect() 算(tests/test_pet_layout_mirror.py 两边对照)。
        #
        # 注意 base 是 package["stage"][:2], **不是** layout["stage_x"]/"stage_y":
        # 后者是舞台上屏幕的落点(129 px 宽的舞台: 单元格里 31, 屏幕上 55), 拿它来减
        # 会让宠物整体左上偏移并被舞台裁掉一角。预览曾经是对的, 设备侧错过一次。
        rect = frame_rect(chosen, package["stage"])
        layer = Image.new("RGBA", (layout["stage_w"], layout["stage_h"]), (0, 0, 0, 0))
        layer.alpha_composite(sprite, (rect["x"], rect["y"]))
        board.paste(layer, (layout["stage_x"], layout["stage_y"]), layer)

        if asleep:
            # 睡眠标记是舞台的子对象, 坐标相对舞台左上角; 设备上是 LV_OPA_70。
            mark = Image.new("RGBA", (layout["stage_w"], layout["stage_h"]), (0, 0, 0, 0))
            ink = self.colors["COL_ACCENT"] + (179,)   # 179/255 ≈ LV_OPA_70
            draw_line(ImageDraw.Draw(mark), self.s["PET_STR_SLEEP_MARK"],
                      self.fonts[16], self.metrics[16], layout["sleep_x"],
                      layout["sleep_y"], ink)
            board.paste(mark, (layout["stage_x"], layout["stage_y"]), mark)

        draw_top_row(board, draw, self.c, self.colors, self.s[status_key], dot,
                     battery_text, self.fonts, self.metrics)
        draw_plan_badge(draw, self.c, self.colors, self.fonts, layout, plan)
        draw_gauges(draw, layout, self.c, self.colors, self.s, self.fonts,
                    self.metrics, limits)
        self.draw_plat_text(draw, self.s[plate_key], layout)
        return finish(board, key)

    def draw_plat_text(self, draw: ImageDraw.ImageDraw, text: str,
                       layout: dict[str, int]) -> None:
        """台身正面的文本块: 两行高, 居中, 宽度 PLAT_TEXT_W。

        一律用 COL_MUTED —— 设备上只有 Bridge 真的下发过文本才转成 COL_INK, 而预览
        画的都是"还没有文本"的占位状态(placeholder_text 那几条)。
        """
        self.block(draw, text, self.fonts[16], self.metrics[16],
                   layout["plat_text_x"], layout["plat_text_w"],
                   layout["plat_text_y"], self.colors["COL_MUTED"])

    def no_pet_screen(self, battery_text: str,
                      plan: str | None = None) -> Image.Image:
        """空槽: 舞台区一句占位(lv_obj_center), 站台上告诉用户去哪儿装。

        恒为离线状态 —— 不画能量槽, 与设备侧"掉线整组隐藏"的语义一致。"""
        board, draw = self.new_board()
        layout = layout_for_stage(self.c["PET_LAYOUT_STAGE_W_REF"],
                                  self.c["PET_LAYOUT_STAGE_H_REF"], self.c)
        paste_platform(board, layout, self.colors)

        self.block(draw, self.s["PET_STR_NO_PET"], self.fonts[16], self.metrics[16],
                   layout["stage_x"], layout["stage_w"],
                   layout["stage_y"]
                   + (layout["stage_h"] - self.metrics[16][0]) // 2,
                   self.colors["COL_MUTED"])

        draw_top_row(board, draw, self.c, self.colors,
                     self.s["PET_STR_STATUS_OFFLINE"], "COL_MUTED", battery_text,
                     self.fonts, self.metrics)
        # 空槽也画徽标: 档位是桥接报的, 与槽里有没有宠物无关。
        draw_plan_badge(draw, self.c, self.colors, self.fonts, layout, plan)
        self.draw_plat_text(draw, self.s["PET_STR_PH_NO_PET"], layout)
        return finish(board, "nopet")

    def transfer_screen(self, pet_id: str, percent: int, message: str,
                        tone: str, battery_text: str) -> Image.Image:
        """传输页: 宠物界面已经拆掉(图集要解除映射), 它顶上, 只有文字和进度条。"""
        board, draw = self.new_board()
        scr_w = self.c["PET_LAYOUT_SCR_W"]

        self.block(draw, self.s["PET_STR_TRANSFER_TITLE"], self.fonts[20],
                   self.metrics[20], 0, scr_w, self.c["TRANS_TITLE_Y"],
                   self.colors["COL_INK"])
        self.block(draw, pet_id, self.fonts[16], self.metrics[16],
                   self.c["TRANS_MSG_X"], scr_w - 2 * self.c["TRANS_MSG_X"],
                   self.c["TRANS_NAME_Y"], self.colors["COL_ACCENT"])

        bar_x, bar_w = self.c["TRANS_BAR_X"], self.c["TRANS_BAR_W"]
        bar_y, bar_h = self.c["TRANS_BAR_Y"], self.c["TRANS_BAR_H"]
        draw.rounded_rectangle((bar_x, bar_y, bar_x + bar_w - 1, bar_y + bar_h - 1),
                               radius=bar_h // 2, fill=self.colors["COL_CARD"],
                               outline=self.colors["COL_CARD_EDGE"], width=1)
        if percent > 0:
            fill_w = max((bar_w - 2) * min(percent, 100) // 100, 1)
            draw.rounded_rectangle((bar_x + 1, bar_y + 1, bar_x + fill_w,
                                    bar_y + bar_h - 2),
                                   radius=(bar_h - 2) // 2,
                                   fill=self.colors["COL_ACCENT"])

        self.block(draw, message, self.fonts[16], self.metrics[16],
                   self.c["TRANS_MSG_X"], self.c["TRANS_MSG_W"],
                   self.c["TRANS_MSG_Y"], self.colors[tone])
        return finish(board, "transfer")

    def prov_screen(self, device: str, pin: str, status: str, tone: str) -> Image.Image:
        """蓝牙配网页: 不透明全屏覆盖层, 配对码是唯一的焦点。"""
        board, draw = self.new_board()
        scr_w = self.c["PET_LAYOUT_SCR_W"]

        self.block(draw, self.s["PET_STR_PROV_TITLE"], self.fonts[20],
                   self.metrics[20], 0, scr_w, self.c["PROV_TITLE_Y"],
                   self.colors["COL_INK"])
        self.block(draw, device, self.fonts[16], self.metrics[16], 0, scr_w,
                   self.c["PROV_NAME_Y"], self.colors["COL_MUTED"])

        card_x, card_w = self.c["PROV_CARD_X"], self.c["PROV_CARD_W"]
        card_y, card_h = self.c["PROV_CARD_Y"], self.c["PROV_CARD_H"]
        draw.rounded_rectangle((card_x, card_y, card_x + card_w - 1,
                                card_y + card_h - 1),
                               radius=16, fill=self.colors["COL_CARD"],
                               outline=self.colors["COL_ACCENT"], width=1)

        # 20 px 的字体下 "1234" 四个数字挤在一起念不清, 设备上拉开成一格一个。
        spaced = " ".join(pin) if len(pin) == 4 else "- - - -"
        self.block(draw, self.s["PET_STR_PROV_PIN_LABEL"], self.fonts[16],
                   self.metrics[16], card_x, card_w,
                   card_y + self.c["PROV_PIN_LABEL_Y"], self.colors["COL_MUTED"])
        self.block(draw, spaced, self.fonts[20], self.metrics[20], card_x, card_w,
                   card_y + self.c["PROV_PIN_Y"], self.colors["COL_ACCENT"])

        self.block(draw, status, self.fonts[16], self.metrics[16],
                   self.c["PROV_STATUS_X"], self.c["PROV_STATUS_W"],
                   self.c["PROV_STATUS_Y"], self.colors[tone])
        self.block(draw, self.s["PET_STR_PROV_HINT"], self.fonts[16],
                   self.metrics[16], 0, scr_w, self.c["PROV_HINT_Y"],
                   self.colors["COL_MUTED"])
        return finish(board, "prov")


# ---------------------------------------------------------------------------
# 找宠物 / 打包
# ---------------------------------------------------------------------------
def find_pet(spec: str) -> Path:
    """--pet 可以是目录、仓库里的 assets/pets/<id>, 或 ~/.codex/pets/<id>。"""
    candidates = [Path(os.path.expanduser(spec)),
                  REPO_ROOT / "assets" / "pets" / spec,
                  Path.home() / ".codex" / "pets" / spec]
    for path in candidates:
        if path.is_dir() and (path / "pet.json").is_file():
            return path
    raise SystemExit(f"找不到宠物 {spec!r}(找过 "
                     + ", ".join(str(p) for p in candidates) + ")")


def load_package(args) -> tuple[dict, Path | None]:
    """返回 (解析好的包, 宠物目录或 None)。"""
    if args.package:
        path = Path(os.path.expanduser(args.package))
        if not path.is_file():
            raise SystemExit(f"没有这个文件: {path}")
        return gen.parse_package(path.read_bytes()), None

    pet_dir = find_pet(args.pet)
    package, _stats = gen.build_package(pet_dir)
    return gen.parse_package(package), pet_dir


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def list_pets() -> None:
    seen: dict[str, list[str]] = {}
    for label, root in (("仓库", REPO_ROOT / "assets" / "pets"),
                        ("Codex", Path.home() / ".codex" / "pets")):
        if root.is_dir():
            for entry in sorted(root.iterdir()):
                if (entry / "pet.json").is_file():
                    seen.setdefault(entry.name, []).append(label)
    if not seen:
        print("没找到任何宠物(pet.json)")
        return
    for name, where in seen.items():
        print(f"  {name:<24} {'/'.join(where)}")


def main() -> int:
    parser = argparse.ArgumentParser(description="渲染宠物界面预览 PNG")
    parser.add_argument("--pet", default="sophie-portrait",
                        help="宠物 id 或目录(默认 sophie-portrait)")
    parser.add_argument("--package", help="直接读一份现成的 .pet, 不用 --pet")
    parser.add_argument("--list", action="store_true", help="列出可用的宠物")
    parser.add_argument("--screen", default="all",
                        help="逗号分隔: all / pet / " + " / ".join(STATIC_SCREENS))
    parser.add_argument("--anim", help="只渲染指定动作的宠物屏(如 idle/working)")
    parser.add_argument("--frame", type=int, default=None, help="指定帧序号")
    parser.add_argument("--battery", default="82%",
                        help="顶栏电量, 如 95%% 或 -- (表示未读到)")
    parser.add_argument("--limits", default="98/31",
                        help="能量槽, 格式 <5h窗已用>/<周窗已用>, 如 98/31; "
                             "某格用 -- 表示未读到")
    parser.add_argument("--plan", default="plus",
                        help="订阅徽标上的档位名(如 plus / pro); "
                             "传空串表示未读到, 徽标不画")
    parser.add_argument("--pin", default="1234", help="配网页上的配对码")
    parser.add_argument("--device", default="CodexPet-75B4",
                        help="配网页上的设备名(CodexPet-MAC 后两字节)")
    parser.add_argument("--transfer-percent", type=int, default=42,
                        help="传输页进度条百分比")
    parser.add_argument("--transfer-failed", action="store_true",
                        help="画传输失败那一屏, 而不是正在接收")
    parser.add_argument("--out-dir", type=Path,
                        help="默认 assets/pets/<pet>/preview, 没有就 build/preview")
    parser.add_argument("--scale", type=int, default=2, help="输出放大倍数")
    args = parser.parse_args()

    if Image is None:
        raise SystemExit("出预览图需要 Pillow(pip install pillow); "
                         "只要版式算术的话见 tests/test_pet_layout_mirror.py")

    if args.list:
        list_pets()
        return 0

    wanted = [s.strip() for s in args.screen.split(",") if s.strip()]
    unknown = [s for s in wanted if s not in ("all", "pet", *STATIC_SCREENS)]
    if unknown:
        parser.error(f"--screen 不认识: {', '.join(unknown)}")
    want_pet = "all" in wanted or "pet" in wanted
    want_static = {s for s in wanted if s in STATIC_SCREENS}
    if "all" in wanted:
        want_static = set(STATIC_SCREENS)

    consts = parse_defines(CONSTANT_SOURCES)
    strings = parse_strings(STRINGS_H)
    metrics = {size: parse_font_metrics(path) for size, path in FONT_C.items()}
    renderer = Renderer(consts, strings, metrics)

    package = None
    pet_dir = None
    if want_pet:
        package, pet_dir = load_package(args)
        print(f"包 {package['id']} ({package['display']}): "
              f"{len(package['frames'])} 帧, {package['blob_size'] / 1024 / 1024:.2f} MiB, "
              f"舞台 {package['stage'][2]}x{package['stage'][3]} @ "
              f"({package['stage'][0]},{package['stage'][1]})")

    # 默认落在宠物自己的 preview/ 下(仓库里那份就是这么存的), 否则去 build/。
    out_dir = args.out_dir
    if out_dir is None:
        local = pet_dir / "preview" if pet_dir is not None else None
        out_dir = local if local is not None and local.is_dir() \
            else REPO_ROOT / "build" / "preview"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"输出目录 {out_dir}")

    images: list[tuple[str, Image.Image]] = []
    limits = parse_limits(args.limits)
    if want_pet and package is not None:
        for scene in SCENES:
            if args.anim and scene[4] != args.anim:
                continue
            images.append((scene[0], renderer.pet_screen(
                scene, package, args.frame, args.battery, limits, args.plan)))

    if "nopet" in want_static:
        images.append(("nopet", renderer.no_pet_screen(args.battery, args.plan)))
    if "transfer" in want_static:
        if args.transfer_failed:
            images.append(("transfer-failed", renderer.transfer_screen(
                args.pet, 0, strings["PET_STR_TRANSFER_FAILED"], "COL_BAD",
                args.battery)))
        else:
            images.append(("transfer", renderer.transfer_screen(
                args.pet, args.transfer_percent, strings["PET_STR_TRANSFER_ENABLING"],
                "COL_MUTED", args.battery)))
    if "prov" in want_static:
        images.append(("prov", renderer.prov_screen(
            args.device, args.pin, strings["PET_STR_PROV_CONNECTED"], "COL_INK")))

    if not images:
        print(f"没有匹配 --screen={args.screen} --anim={args.anim} 的屏", file=sys.stderr)
        return 1

    for key, image in images:
        if args.scale > 1:
            image = image.resize((image.width * args.scale, image.height * args.scale),
                                 Image.NEAREST)
        path = out_dir / f"screen-{key}.png"
        image.save(path)
        print(f"  -> {path}")

    # 全部拼成一张联系表, 方便一眼比较。屏多了就排成方阵, 免得拉到几千像素宽。
    thumbnails = [Image.open(out_dir / f"screen-{key}.png") for key, _ in images]
    columns = len(thumbnails) if len(thumbnails) <= 6 else math.ceil(math.sqrt(len(thumbnails)))
    gap = 12
    rows = math.ceil(len(thumbnails) / columns)
    sheet = Image.new("RGB", (columns * thumbnails[0].width + gap * (columns - 1),
                              rows * thumbnails[0].height + gap * (rows - 1)),
                      (0, 0, 0))
    for index, thumb in enumerate(thumbnails):
        sheet.paste(thumb, ((index % columns) * (thumb.width + gap),
                            (index // columns) * (thumb.height + gap)))
    sheet_path = out_dir / "screen-all.png"
    sheet.save(sheet_path)
    print(f"  -> {sheet_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
