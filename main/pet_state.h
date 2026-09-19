// main/pet_state.h
// 宠物状态机:把「桥接连路状态 + Codex 状态」映射成图集里的动画行。
//
// 本文件刻意不依赖 ESP-IDF 与 LVGL, 这样能在主机上直接跑单元测试
// (tests/test_pet_state.c), 也便于在真机上单独排查状态跳转问题。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Codex 侧报告的运行状态, 由 Mac 上的 Pet Bridge 通过 Wi-Fi 下发。
typedef enum {
    PET_CODEX_IDLE = 0,   // 空闲, 没有任务在跑
    PET_CODEX_WORKING,    // 正在执行任务
    PET_CODEX_WAITING,    // 等待用户确认或输入
    PET_CODEX_READY,      // 产出已完成, 可以查看
    PET_CODEX_FAILED,     // 阻塞 / 失败 / 被取消
    PET_CODEX_COUNT,
} pet_codex_state_t;

// 与 Pet Bridge 的链路状态。
typedef enum {
    PET_LINK_OFFLINE = 0,  // 未连接(或刚断开): 宠物睡觉
    PET_LINK_CONNECTING,   // 正在连 Wi-Fi / TCP
    PET_LINK_ONLINE,       // 已连接, 状态实时同步
    PET_LINK_COUNT,
} pet_link_state_t;

// 图集里的动画行。顺序与 pet_atlas_sophie_portrait.h 的 PET_ATLAS_STATES 一致。
typedef enum {
    PET_ANIM_IDLE = 0,
    PET_ANIM_RUNNING_RIGHT,
    PET_ANIM_RUNNING_LEFT,
    PET_ANIM_WAVING,
    PET_ANIM_JUMPING,
    PET_ANIM_FAILED,
    PET_ANIM_WAITING,
    PET_ANIM_RUNNING,
    PET_ANIM_REVIEW,
    PET_ANIM_COUNT,
} pet_anim_t;

// 当前应当播放的姿态。动画驱动器只消费这个结构, 不关心状态来源。
typedef struct {
    pet_anim_t anim;      // 正在循环播放的动画行
    bool       once;      // 先播一轮 anim, 结束后自动切到 next
    pet_anim_t next;      // 仅在 once 为真时有意义
    bool       asleep;    // 断线睡眠: 降低不透明度并放慢帧速
    uint16_t   speed_pct; // 帧时长倍率, 100 = 按图集原始时长
} pet_pose_t;

typedef struct {
    pet_link_state_t  link;
    pet_codex_state_t codex;
    pet_anim_t        anim;      // 当前正在播放的动画行
    bool              transition; // anim 刚刚变化, 需要从第 0 帧重新开始
    bool              once;       // 当前 anim 是一次性播放
    pet_anim_t        next;
} pet_state_t;

// 睡眠时把帧时长放大到 250%, 让呼吸动作看起来像睡觉而不是待机。
#define PET_STATE_SLEEP_SPEED_PCT 250

void pet_state_init(pet_state_t *state);

// 更新链路状态。返回 true 表示动画行发生了变化(调用方需要重置帧序号)。
bool pet_state_set_link(pet_state_t *state, pet_link_state_t link);

// 更新 Codex 状态。返回 true 表示动画行发生了变化。
bool pet_state_set_codex(pet_state_t *state, pet_codex_state_t codex);

// 一次性动画播完一轮后由动画驱动器调用。
// 返回 true 表示姿态发生了变化, 需要重新装载帧。
bool pet_state_oneshot_complete(pet_state_t *state);

// 取当前应播放的姿态。
pet_pose_t pet_state_pose(const pet_state_t *state);

// Codex 状态名(用于日志与协议回显)。越界返回 "idle"。
const char *pet_codex_state_name(pet_codex_state_t state);

// 动画行名(用于日志)。越界返回 "?"。
const char *pet_anim_name(pet_anim_t anim);

// 解析协议里的状态名。无法识别时返回 false, 且不修改 *out。
bool pet_codex_state_parse(const char *name, pet_codex_state_t *out);
