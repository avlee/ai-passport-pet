#!/usr/bin/env python3
"""把提示音源文件(assets/music/*.wav)转成可编译进固件的 C 数组。

设备播放器只认一种格式: 16 kHz / 单声道 / 16-bit PCM 小端。任何一步不符合都直接
报错退出 —— 坏格式宁可死在生成时, 也不要上真机才发现音调不对、速度快一倍。

输出: assets/music/pet_sound_clips.c, 由 main/CMakeLists.txt 的 target_sources
追加进组件(与 assets/fonts 的字库同一个姿势)。
重新生成: python3 tools/gen_pet_sounds.py
"""

import struct
import wave
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT = REPO_ROOT / "assets" / "music" / "pet_sound_clips.c"

REQUIRED_RATE = 16000

# (片段名, 相对仓库根的源文件)。固件里 pet_sound.c 按名字查找, 两边要一致。
CLIPS = [
    ("taskdone", "assets/music/taskdone.wav"),
]


def read_pcm(path: Path) -> list[int]:
    with wave.open(str(path), "rb") as audio:
        if audio.getcomptype() != "NONE":
            raise SystemExit(f"{path}: 需要无压缩 PCM, 实际是 {audio.getcomptype()}")
        if audio.getsampwidth() != 2:
            raise SystemExit(
                f"{path}: 需要 16-bit 采样, 实际 {audio.getsampwidth() * 8}-bit")
        if audio.getnchannels() != 1:
            raise SystemExit(
                f"{path}: 需要单声道, 实际 {audio.getnchannels()} 声道")
        rate = audio.getframerate()
        if rate != REQUIRED_RATE:
            raise SystemExit(f"{path}: 需要 {REQUIRED_RATE} Hz, 实际 {rate} Hz")
        frames = audio.readframes(audio.getnframes())
    return list(struct.unpack(f"<{len(frames) // 2}h", frames))


def emit_clip(out: list[str], name: str, path: Path) -> None:
    samples = read_pcm(path)
    seconds = len(samples) / REQUIRED_RATE
    out.append(f"// {path.name}: {seconds:.2f} s, {REQUIRED_RATE} Hz, mono, "
               f"16-bit PCM, {len(samples)} 采样")
    out.append(f"const int16_t pet_sound_{name}_pcm[{len(samples)}] = {{")
    for start in range(0, len(samples), 12):
        row = samples[start:start + 12]
        out.append("    " + ", ".join(str(value) for value in row) + ",")
    out.append("};")
    out.append(f"const size_t pet_sound_{name}_len = {len(samples)};  // 采样数, 不是字节")
    out.append("")


def main() -> None:
    out = [
        "// 由 tools/gen_pet_sounds.py 生成 —— **不要手改**。",
        "// 重新生成: python3 tools/gen_pet_sounds.py",
        "//",
        "// 数据是 16 kHz / 单声道 / 16-bit PCM, 播放器见 main/pet_sound.c。",
        "// 常量放在 flash 的 rodata 里, 播放时流式分块喂给 I2S, 不整段进 RAM",
        "// (C3 没有 PSRAM, 整段 1.5 秒就要 49 KB, 赌不起)。",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
    ]
    for name, rel in CLIPS:
        emit_clip(out, name, REPO_ROOT / rel)
    OUTPUT.write_text("\n".join(out), encoding="utf-8")
    print(f"已生成 {OUTPUT}")


if __name__ == "__main__":
    main()
