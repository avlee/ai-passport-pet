// main/pet_ui.h
// Codex 宠物界面: 一块直接立在背景上的宠物舞台 + 状态标签 + 站台(承载文本)
// + 信息面板 + 蓝牙配网页 + 宠物传输页。
//
// 设计目标(用户第一条要求): 屏幕上的宠物要和 ChatGPT App 里的看起来一致,
// 且动画流畅。为此:
//   1. 帧一律按图集原始像素 1:1 绘制, 不做缩放 —— 缩放既糊又费 CPU,
//      而图集的裁剪区(129x198)在 240x320 屏上本来就是合适的尺寸。
//   2. 每帧的落点用图集里的裁剪原点(frame->x/y)算出来, 所以动作的位移
//      来自素材本身, 与 ChatGPT 里的表现一致, 不是我们另加的动画。
//   3. 帧间隔直接取图集里的 duration, 不额外插值, 也不做固定帧率。
//
// 版式: 舞台尺寸现在是**运行时**的(每只宠物图集的裁剪区并集不同), 所以坐标不再
// 是这里的常量, 而是 main/pet_layout.c 按当前宠物算出来的 pet_layout_t。
//
// 线程约定: 本文件的每个函数都会自己加 LVGL 锁, 因此可以从任意任务调用。
// 那把锁是递归互斥量(esp_lvgl_port 用 xSemaphoreCreateRecursiveMutex), 所以
// 从已持锁的路径里再调一次是安全的 —— 文件内部因此仍保留 *_locked 版本,
// 只是为了避免重复取锁的开销, 不是因为会死锁。
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// 是否逐帧渲染宠物。息屏(背光 0)时由 pet_app 关掉: 看不见的画面没必要每帧都把
// 精灵经 SPI 推给面板。只影响重绘节奏, 不动任何状态 —— 亮屏后从当前帧接着播。
// 屏幕开关本身不在这里管, 真值在 pet_app(见 pet_screen.h)。
void pet_ui_set_anim_enabled(bool enabled);

// 链路状态。断线时宠物进入睡眠(压暗 + 放慢), 见 pet_state.h。
void pet_ui_set_link(pet_link_state_t link);

// Codex 状态。
void pet_ui_set_codex(pet_codex_state_t codex);

// Codex 输出的文本。传 NULL 或空串则显示当前状态的占位文案。
void pet_ui_set_text(const char *utf8);

// 电量百分比, 0..100;-1 表示读不到。
void pet_ui_set_battery(int soc_percent);

// Codex 用量限额(屏幕两侧的竖形能量槽)。两个参数都是**已用**百分比, 0..100;
// 小于 0 表示该窗口没有快照, 对应的槽隐藏。左侧 = 5 小时窗, 右侧 = 周窗。
void pet_ui_set_limits(int primary_used, int weekly_used);

// 信息面板要展示的连接参数。
void pet_ui_set_settings(const pet_settings_t *settings);

// 立刻播一次一次性动画(用户按键互动), 播完自动回到同步状态。
void pet_ui_emote(pet_anim_t anim);

// 信息面板显隐。
void pet_ui_set_info_visible(bool visible);
bool pet_ui_info_visible(void);

// ---------------------------------------------------------------------------
// 蓝牙配网页
// ---------------------------------------------------------------------------
// 一条不透明的全屏覆盖层: 设备还没联网时宠物本来就在睡眠, 让配对码成为画面上
// 唯一的焦点更好读。它和宠物界面共用同一块屏, 靠显隐切换。

// 状态文字的语气, 决定用哪种颜色。
typedef enum {
    PET_TONE_INFO = 0,  // 普通说明
    PET_TONE_GOOD,      // 成功
    PET_TONE_BAD,       // 失败
} pet_tone_t;

void pet_ui_set_provision_visible(bool visible);

// 更新配网页内容。pin 不是 4 位时显示破折号占位, status 为空则不显示。
void pet_ui_set_provision(const char *device_name, const char *pin,
                          const char *status, pet_tone_t tone);

// 当前正在播放的动画行(调试/日志用)。
pet_anim_t pet_ui_current_anim(void);

// ---------------------------------------------------------------------------
// 宠物传输页
// ---------------------------------------------------------------------------
// 换宠物的第一步必须把图集从 flash 上解除映射 —— 一边 mmap 一边擦写同一块 flash
// 是未定义行为 —— 而宠物界面上正挂着指向那块 mmap 的图像描述符。所以只能先把宠物
// 界面整个拆掉。
//
// 拆掉之后不能留一块黑屏: 那几秒钟用户完全不知道设备在干什么, 拔电就废了半份包。
// 于是这里另建一块只有文字和进度条的屏幕顶上, 传完由调用方
// pet_atlas_load() + pet_ui_build() 重建宠物界面。

// 拆掉宠物界面, 换成传输页。total_bytes 为 0 时进度条按不确定处理。
void pet_ui_begin_transfer(const char *pet_id, uint32_t total_bytes);

// 更新进度。没在传输时是空操作。
void pet_ui_set_transfer_progress(uint32_t received_bytes);

// 改传输页底部那行说明(如"正在启用…"/失败原因)。没在传输时是空操作。
void pet_ui_set_transfer_message(const char *text, pet_tone_t tone);

// 拆掉传输页。调用方随后负责 pet_ui_build()。
void pet_ui_end_transfer(void);

// 是否正停在传输页上。
bool pet_ui_transferring(void);
