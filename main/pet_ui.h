// main/pet_ui.h
// Codex 宠物界面: 一块全屏的宠物舞台 + 状态标签 + 文本气泡 + 信息面板。
//
// 设计目标(用户第一条要求): 屏幕上的宠物要和 ChatGPT App 里的看起来一致,
// 且动画流畅。为此:
//   1. 帧一律按图集原始像素 1:1 绘制, 不做缩放 —— 缩放既糊又费 CPU,
//      而图集的裁剪区(129x198)在 240x320 屏上本来就是合适的尺寸。
//   2. 每帧的落点用图集里的裁剪原点(frame->x/y)算出来, 所以动作的位移
//      来自素材本身, 与 ChatGPT 里的表现一致, 不是我们另加的动画。
//   3. 帧间隔直接取图集里的 duration, 不额外插值, 也不做固定帧率。
//
// 线程约定: 本文件的每个函数都会自己加 LVGL 锁, 因此可以从任意任务调用,
// 但【不能在已持有 LVGL 锁时调用】(锁不可重入)。
#pragma once

#include <stdbool.h>

#include "pet_settings.h"
#include "pet_state.h"

// 创建界面并启动动画定时器。重复调用是空操作。
void pet_ui_build(void);

// 复位模块内部状态(链路、Codex、文案、电量)。在第一次 build 之前调用一次。
// 与 build/destroy 分开, 是为了让「销毁界面 -> 进演示菜单 -> 重建界面」这条
// 路径不丢已经同步到的状态。
void pet_ui_init(void);

// 销毁界面、停止定时器。重复调用是空操作。
void pet_ui_destroy(void);

// 界面是否已经建好。
bool pet_ui_ready(void);

// 链路状态。断线时宠物进入睡眠(压暗 + 放慢), 见 pet_state.h。
void pet_ui_set_link(pet_link_state_t link);

// Codex 状态。
void pet_ui_set_codex(pet_codex_state_t codex);

// Codex 输出的文本。传 NULL 或空串则显示当前状态的占位文案。
void pet_ui_set_text(const char *utf8);

// 电量百分比, 0..100;-1 表示读不到。
void pet_ui_set_battery(int soc_percent);

// 信息面板要展示的连接参数。
void pet_ui_set_settings(const pet_settings_t *settings);

// 立刻播一次一次性动画(用户按键互动), 播完自动回到同步状态。
void pet_ui_emote(pet_anim_t anim);

// 信息面板显隐。
void pet_ui_set_info_visible(bool visible);
bool pet_ui_info_visible(void);

// 当前正在播放的动画行(调试/日志用)。
pet_anim_t pet_ui_current_anim(void);
