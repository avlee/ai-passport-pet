#!/usr/bin/env python3
"""把 ~/.codex/pets/<id> 里的一份 v2 图集打包成设备能直接收下的 .pet 包。

以前这里的做法是**生成 C 源码**: 把逐帧像素拼成一个 .bin 用 EMBED_FILES 编进固件,
再生成帧表的 .c/.h。换一只宠物等于改三处源码 + 重新编译 + 重新烧录, 而且 8 MB
flash 被 app 吃满之后, 连"多带一只"都不可能(A 槽里剩下的 2.42 MB 比最瘦的一只
还少 15 KB)。

现在产物是一份**自描述的数据包**, 由 Pet Bridge 从 Mac 侧推到设备上那个 3.94 MB 的
`pets` 分区, 设备侧不需要重新编译任何东西。

包布局(小端, 全部 4 字节对齐), 与 main/pet_pkg.h 一一对应:

    +0                头部, 120 字节
    +frames_offset    帧表,   frame_count x 16
    +states_offset    状态表, state_count  x 20
    +blob_offset      逐帧 RGB565A8 像素

三块内容各有一条 CRC32: 头部 / 表区(帧表+状态表连成一段) / 像素。设备在
pet_slot_finish() 里核对整包 CRC, 之后每次开机再核对结构与表区 CRC。

用法:
    python3 tools/gen_pet_package.py --pet sophie-portrait
    python3 tools/gen_pet_package.py --pet ~/.codex/pets/sophie --out /tmp/s.pet
    python3 tools/gen_pet_package.py --list
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import TYPE_CHECKING

# Pillow 只在"从图集切帧"那一步用得到。放成惰性导入是为了让 assemble_package()
# 与其测试(tests/test_pet_package.py)在没有 Pillow 的解释器上也能跑 —— 那一步
# 才是与 C 侧结构体布局对接的关键部分。
if TYPE_CHECKING:
    from PIL import Image

# ---- v2 图集几何 (docs/development/engineering/codex-pet.zh_CN.md) -----------
CELL_W, CELL_H = 192, 208
COLS, ROWS = 8, 11
SUPPORTED_SPRITE_VERSION = 2

# 行名与顺序必须与 main/pet_state.h 的 pet_anim_t 完全一致 —— pet_pkg.c 里有一张
# 同样的表在逐条核对。顺序错了不会崩, 只会让"工作中"播出"挥手", 所以两边都钉死。
# (名字, 行号, 帧数)                帧数来自 docs 里的 v2 规范
STATES: list[tuple[str, int, int]] = [
    ("idle", 0, 6),
    ("running_right", 1, 8),
    ("running_left", 2, 8),
    ("waving", 3, 4),
    ("jumping", 4, 5),
    ("failed", 5, 8),
    ("waiting", 6, 6),
    ("running", 7, 6),
    ("review", 8, 6),
]

# 每行的逐帧时长。波形来自素材规范: 循环动作最后一帧停久一点, 看起来才不"抖"。
DURATIONS: dict[str, list[int]] = {
    "idle": [280, 110, 110, 140, 140, 320],
    "running_right": [120] * 7 + [220],
    "running_left": [120] * 7 + [220],
    "waving": [140] * 3 + [280],
    "jumping": [140] * 4 + [280],
    "failed": [140] * 7 + [240],
    "waiting": [150] * 5 + [260],
    "running": [120] * 5 + [220],
    "review": [150] * 5 + [280],
}

# 低于这个 alpha 的像素是 webp 在透明区里的压缩噪点。不抹掉的话, 紧裁的包围盒
# 会被这些看不见的"鬼影"撑大, 平均白白多出几 KB 一帧。
ALPHA_FLOOR = 8

# 与 main/pet_pkg.h 的一致校验。两边任何一处动了, 这里必须跟着动。
MAGIC = 0x31544550       # "PET1"
VERSION = 1
HEADER_SIZE = 120
ID_MAX = 32
NAME_MAX = 32
STATE_NAME_MAX = 16
STATE_COUNT = len(STATES)

# 与 main/pet_layout.h 的可用范围一致。放不下的包设备会直接拒收(pet_atlas_load
# 会调 pet_layout_stage_supported), 所以在这里就报出来, 别等推上去才发现。
STAGE_W_MIN, STAGE_W_MAX = 64, 224
STAGE_H_MIN, STAGE_H_MAX = 64, 206


def find_pet_dir(spec: str) -> Path:
    """--pet 既可以是 id, 也可以是目录(绝对或相对)。"""
    path = Path(os.path.expanduser(spec))
    if path.is_dir():
        return path
    candidate = Path.home() / ".codex" / "pets" / spec
    if candidate.is_dir():
        return candidate
    raise SystemExit(f"找不到宠物 {spec!r}: 既不是目录, 也不在 {candidate.parent}")


def load_manifest(pet_dir: Path) -> dict:
    manifest_path = pet_dir / "pet.json"
    if not manifest_path.is_file():
        raise SystemExit(f"{pet_dir} 里没有 pet.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    version = manifest.get("spriteVersionNumber")
    if version != SUPPORTED_SPRITE_VERSION:
        raise SystemExit(
            f"{manifest_path}: 只支持 spriteVersionNumber={SUPPORTED_SPRITE_VERSION}, "
            f"这里是 {version!r}")

    pet_id = manifest.get("id") or pet_dir.name
    encoded = pet_id.encode("utf-8")
    if len(encoded) + 1 > ID_MAX:
        raise SystemExit(f"宠物 id {pet_id!r} 太长(上限 {ID_MAX - 1} 字节)")
    display = manifest.get("displayName") or pet_id
    if len(display.encode("utf-8")) + 1 > NAME_MAX:
        raise SystemExit(f"displayName {display!r} 太长(上限 {NAME_MAX - 1} 字节)")

    sheet = pet_dir / manifest.get("spritesheetPath", "spritesheet.webp")
    if not sheet.is_file():
        raise SystemExit(f"{manifest_path} 指的图集不存在: {sheet}")

    return {"id": pet_id, "display": display, "sheet": sheet}


def rgb565a8(cell: "Image.Image") -> bytes:
    """RGB565 小端颜色平面 + 8 位 alpha 平面。与 lv_image_dsc_t 的 RGB565A8 一致。"""
    colour = bytearray()
    for r, g, b in cell.convert("RGB").get_flattened_data():
        colour += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(colour) + cell.getchannel("A").tobytes()


def build_frames(sheet_path: Path) -> tuple[bytes, list[dict], list[dict]]:
    """切帧并打包像素。返回 (blob, 帧表, 状态表)。"""
    from PIL import Image  # 见文件头的惰性导入说明

    atlas = Image.open(sheet_path).convert("RGBA")
    expected = (COLS * CELL_W, ROWS * CELL_H)
    if atlas.size != expected:
        raise SystemExit(f"{sheet_path} 必须是 {expected[0]}x{expected[1]}, "
                         f"实际 {atlas.size[0]}x{atlas.size[1]}")

    atlas.putalpha(atlas.getchannel("A").point(lambda v: 0 if v < ALPHA_FLOOR else v))

    blob = bytearray()
    frames: list[dict] = []
    states: list[dict] = []

    for name, row, count in STATES:
        durations = DURATIONS[name]
        if len(durations) != count:
            raise SystemExit(f"{name}: 帧数与时长表对不上({count} vs {len(durations)})")

        first = len(frames)
        for col in range(count):
            cell = atlas.crop((col * CELL_W, row * CELL_H,
                               (col + 1) * CELL_W, (row + 1) * CELL_H))
            bbox = cell.getchannel("A").getbbox()
            if bbox is None:
                raise SystemExit(f"{name} 第 {col} 帧是全透明的 —— 图集切错了")
            crop = cell.crop(bbox)

            # 每帧起点 4 字节对齐: 设备直接按 uint16 读 mmap 出来的 flash, 错位读
            # 在 ESP32-C3 上要么慢得离谱要么直接异常。
            blob += b"\x00" * ((-len(blob)) % 4)
            frames.append({
                "offset": len(blob),
                "x": bbox[0], "y": bbox[1],
                "w": crop.width, "h": crop.height,
                "duration": durations[col],
            })
            blob += rgb565a8(crop)

        states.append({"name": name, "first": first, "count": count})

    return bytes(blob), frames, states


def assemble_package(pet_id: str, display: str, blob: bytes,
                     frames: list[dict], states: list[dict]) -> tuple[bytes, dict]:
    """把像素与两张表拼成一个完整的 .pet。

    刻意与"从图集切帧"分开: 这一段的每个字节都对着 main/pet_pkg.h 的结构体布局,
    是最容易"两边悄悄错开"的地方 —— 错开了也不会编译失败, 只会让设备拒收或者
    画出乱码。分开之后 tests/test_pet_package.py 可以不带 Pillow 直接调它, 把
    生成出来的字节灌进设备侧的解析器逐字段比对。
    """
    stage_x0 = min(f["x"] for f in frames)
    stage_y0 = min(f["y"] for f in frames)
    stage_w = max(f["x"] + f["w"] for f in frames) - stage_x0
    stage_h = max(f["y"] + f["h"] for f in frames) - stage_y0

    frames_offset = HEADER_SIZE
    states_offset = frames_offset + len(frames) * 16
    blob_offset = states_offset + len(states) * 20

    frame_bytes = b"".join(
        # pet_pkg_frame_t: uint32 offset + 4 x int16 + uint16 duration + uint16 reserved
        struct.pack("<IhhhhHH", f["offset"], f["x"], f["y"], f["w"], f["h"],
                    f["duration"], 0)
        for f in frames
    )
    state_bytes = b"".join(
        # pet_pkg_state_t: char name[16] + uint16 first + uint16 count
        struct.pack("<16sHH", s["name"].encode("ascii"), s["first"], s["count"])
        for s in states
    )

    tables = frame_bytes + state_bytes
    total_size = blob_offset + len(blob)

    header = struct.pack(
        "<IHHIIIIIII"  # magic version header_size total blob_size blob_crc tables_crc
                       # frames_offset states_offset blob_offset
        "HHHHHHHH"     # frame_count state_count cell_w cell_h stage_x stage_y
                       # stage_w stage_h
        "32s32sI",     # pet_id display_name header_crc32
        MAGIC, VERSION, HEADER_SIZE, total_size,
        len(blob), zlib.crc32(blob), zlib.crc32(tables),
        frames_offset, states_offset, blob_offset,
        len(frames), len(states), CELL_W, CELL_H,
        stage_x0, stage_y0, stage_w, stage_h,
        pet_id.encode("utf-8"), display.encode("utf-8"),
        0,             # header_crc32: 先占位
    )
    assert len(header) == HEADER_SIZE, len(header)

    # 头部 CRC 的口径与设备侧 header_crc32() 一致: 本字段按 0 参与计算。
    crc = zlib.crc32(header) & 0xFFFFFFFF
    package = header[:-4] + struct.pack("<I", crc) + tables + blob
    assert len(package) == total_size, (len(package), total_size)

    stats = {
        "id": pet_id, "display": display,
        "frames": len(frames), "blob": len(blob), "total": total_size,
        "stage": (stage_x0, stage_y0, stage_w, stage_h), "crc32": crc,
        "frames_offset": frames_offset, "states_offset": states_offset,
        "blob_offset": blob_offset,
    }
    return package, stats


def parse_package(data: bytes) -> dict:
    """把一份 .pet 拆回字段、帧表与像素, 顺便核对三条 CRC。

    assemble_package() 的反向操作。预览工具靠它读"设备真正会收到的那份字节" ——
    从包读而不是从源图集重切一遍, 这样预览图验证的就是传输链路上的实际内容: 打包时
    某个偏移算错, 画出来立刻缺一块或错位。

    长度以包内声明为准(设备上 mmap 的是整块 3.94 MB 分区, 尾巴上全是 0xFF), 所以
    这里也允许外面多给字节。
    """
    if len(data) < HEADER_SIZE:
        raise SystemExit(f".pet 太短: {len(data)} 字节, 连 {HEADER_SIZE} 字节的头部都不够")

    header = struct.unpack_from("<IHHIIIIIIIHHHHHHHH32s32sI", data, 0)
    (magic, version, header_size, total_size, blob_size, blob_crc, tables_crc,
     frames_offset, states_offset, blob_offset,
     frame_count, state_count, cell_w, cell_h,
     stage_x, stage_y, stage_w, stage_h,
     raw_id, raw_name, header_crc) = header

    if magic != MAGIC:
        raise SystemExit("不是宠物包: 魔术字对不上(分区是空的, 或者文件传坏了)")
    if version != VERSION:
        raise SystemExit(f"宠物包版本 {version} 不是 {VERSION}")
    if header_size != HEADER_SIZE:
        raise SystemExit(f"头部长度 {header_size} 不是 {HEADER_SIZE}")
    if total_size > len(data):
        raise SystemExit(f"包声明 {total_size} 字节, 实际只给了 {len(data)} 字节")
    if total_size != blob_offset + blob_size:
        raise SystemExit(f"声明长度 {total_size} 与 blob 末尾 {blob_offset + blob_size} 不符")

    # 与设备侧 header_crc32() 同口径: 本字段按 0 参与计算。
    expected = zlib.crc32(data[:HEADER_SIZE - 4] + b"\x00\x00\x00\x00") & 0xFFFFFFFF
    if header_crc != expected:
        raise SystemExit(f"头部 CRC 不对: 包内 {header_crc:08x}, 实算 {expected:08x}")

    tables = data[frames_offset:blob_offset]
    if zlib.crc32(tables) & 0xFFFFFFFF != tables_crc:
        raise SystemExit("表区(帧表+状态表) CRC 不对")
    blob = data[blob_offset:blob_offset + blob_size]
    if zlib.crc32(blob) & 0xFFFFFFFF != blob_crc:
        raise SystemExit("像素数据 CRC 不对")

    frames = []
    for index in range(frame_count):
        offset, x, y, w, h, duration, _reserved = struct.unpack_from(
            "<IhhhhHH", data, frames_offset + index * 16)
        frames.append({"offset": offset, "x": x, "y": y, "w": w, "h": h,
                       "duration": duration})

    states = []
    for index in range(state_count):
        raw, first, count = struct.unpack_from("<16sHH", data, states_offset + index * 20)
        states.append({"name": raw.split(b"\x00", 1)[0].decode("ascii"),
                       "first": first, "count": count})

    return {
        "id": raw_id.split(b"\x00", 1)[0].decode("utf-8"),
        "display": raw_name.split(b"\x00", 1)[0].decode("utf-8"),
        "frames": frames, "states": states, "blob": blob,
        "cell": (cell_w, cell_h),
        "stage": (stage_x, stage_y, stage_w, stage_h),
        "blob_size": blob_size, "total_size": total_size,
    }


def build_package(pet_dir: Path) -> tuple[bytes, dict]:
    meta = load_manifest(pet_dir)
    blob, frames, states = build_frames(meta["sheet"])

    stage_w = max(f["x"] + f["w"] for f in frames) - min(f["x"] for f in frames)
    stage_h = max(f["y"] + f["h"] for f in frames) - min(f["y"] for f in frames)
    if not (STAGE_W_MIN <= stage_w <= STAGE_W_MAX) or not (STAGE_H_MIN <= stage_h <= STAGE_H_MAX):
        raise SystemExit(
            f"{meta['id']}: 舞台 {stage_w}x{stage_h} 放不下这块 240x320 的屏 "
            f"(允许 {STAGE_W_MIN}..{STAGE_W_MAX} x {STAGE_H_MIN}..{STAGE_H_MAX}); "
            f"设备会拒收这只宠物。见 main/pet_layout.h")

    package, stats = assemble_package(meta["id"], meta["display"], blob, frames, states)
    stats["sheet"] = meta["sheet"]
    return package, stats


def list_pets(pets_dir: Path) -> list[Path]:
    if not pets_dir.is_dir():
        return []
    found = []
    for entry in sorted(pets_dir.iterdir()):
        if entry.is_dir() and (entry / "pet.json").is_file():
            found.append(entry)
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description="生成设备用的 .pet 宠物包")
    ap.add_argument("--pet", help="宠物 id 或 ~/.codex/pets 下的目录")
    ap.add_argument("--out", type=Path, help="输出路径(默认 build/pets/<id>.pet)")
    ap.add_argument("--list", action="store_true", help="列出可用的宠物")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    pets_dir = Path.home() / ".codex" / "pets"

    if args.list:
        for entry in list_pets(pets_dir):
            try:
                meta = load_manifest(entry)
                print(f"  {meta['id']:<24} {meta['display']}")
            except SystemExit as exc:
                print(f"  {entry.name:<24} (不可用: {exc})")
        return 0

    if not args.pet:
        ap.error("--pet 或 --list 二选一")

    packet, stats = build_package(find_pet_dir(args.pet))

    out = args.out or (repo_root / "build" / "pets" / f"{stats['id']}.pet")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(packet)

    print(f"{stats['id']} ({stats['display']})")
    print(f"  {stats['frames']} 帧, blob {stats['blob']:,} 字节, "
          f"整包 {stats['total']:,} 字节 ({stats['total'] / 1024 / 1024:.2f} MiB)")
    print(f"  舞台 {stats['stage'][2]}x{stats['stage'][3]} @ "
          f"({stats['stage'][0]},{stats['stage'][1]})")
    print(f"  header crc32 {stats['crc32']:08x}")
    print(f"  -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
