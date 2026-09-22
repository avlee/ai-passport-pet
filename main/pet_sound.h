// main/pet_sound.h
// 提示音播放器: Codex 任务完成时播一段"庆祝音效 + 任务完成"的短音频。
//
// 数据源是固件内嵌的 PCM 常量(assets/music/pet_sound_clips.c, 由
// tools/gen_pet_sounds.py 从 .wav 生成), 播放时流式分块喂给 I2S —— 不整段进 RAM
// (C3 没有 PSRAM, 整段 1.5 秒就要 49 KB, 赌不起)。
//
// 线程约定: pet_sound_play() 任何线程都能调, 内部只发一个任务通知, 真正的
// 音频写入在自己的任务里做(与 demo_audio 相同的姿势 —— 音频写是阻塞的,
// 不能占着 bridge 回调或 LVGL 任务的线程)。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 创建播放任务。失败(内存不足)时返回错误码, 此时 play() 变成空操作 ——
// 没有提示音不是致命伤, 宠物应用必须照常起来。
esp_err_t pet_sound_start(void);

// 请求播放指定片段。clip 不认识时静默忽略(主机可以比固件新); 播放过程中
// 再来一次请求会从头重播, 不排队 —— 提示音堆积起来只会变成噪音。
void pet_sound_play(const char *clip);

// 演示菜单期间整段静音: demo_audio 也在操作同一颗 ES8311, 两条流混写只会
// 出杂音。菜单退出后恢复。
void pet_sound_set_enabled(bool enabled);
