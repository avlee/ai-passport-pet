// main/pet_screen.h
// 屏幕电源策略: 空闲自动息屏, 有新内容自动亮屏。
//
// 为什么单独做成一个模块, 而不是散在 pet_app 里:
//   1. 这套判定是纯逻辑 —— 输入只有"现在几点"和"有没有新东西", 与 ESP-IDF、LVGL
//      都无关, 所以能在主机上把边界逐条钉住(tests/test_pet_screen.c)。判错的代价
//      是两个方向都很难受: 该黑不黑(费电, 正是当初要解决的问题)或者人还没看一眼
//      就黑了。两条都值得有测试。
//   2. "背光给多少"只能有一个出口。输入来自三处(链路变化、Bridge 消息、按键),
//      各写各的结果是"谁最后写谁赢", 排查时得同时盯三处。
//
// 时间一律传绝对时刻(微秒, 与 esp_timer_get_time() 同源), 模块自己不取时间 ——
// 于是主机测试可以随便跳跃, 不用真的睡一分钟。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PET_SCREEN_ON = 0,   // 亮屏
    PET_SCREEN_OFF,      // 息屏(背光 0)
} pet_screen_state_t;

typedef struct {
    pet_screen_state_t state;
    int64_t            last_activity_us;  // 最后一次"有新内容"的时刻
    bool               hold;              // true = 有必须看得见的画面, 不自动息屏
    bool               user_lock;         // true = 用户手动锁定, 亮屏只听按键的
} pet_screen_t;

// 开机: 亮屏, 从现在起算空闲。
void pet_screen_init(pet_screen_t *screen, int64_t now_us);

// 有新内容了(收到 Bridge 消息 / 按下按键 / 链路恢复): 亮屏并重新计时。
// 用户锁定期间是空操作 —— 锁定的本意就是"有消息也不亮屏"。
void pet_screen_touch(pet_screen_t *screen, int64_t now_us);

// 持有/释放"必须看得见"的窗口(蓝牙配网页、宠物接收页、演示菜单)。
// 开始持有会立刻亮屏。开始与结束**都**算一次活动: 结束那一刻屏幕上刚换成新画面,
// 不能拿旧的空闲时刻把它当场关掉。锁定期间不亮屏(hold 照常记录)。
void pet_screen_set_hold(pet_screen_t *screen, bool hold, int64_t now_us);

// 用户锁定/解锁(双击确定息屏, 任意键解锁)。锁定立刻息屏; 之后 touch()/set_hold()
// 都不再亮屏, 只有解锁会亮 —— 解锁本身就是一次 touch(重置空闲计时)。
// 调用侧负责保证锁定不发生在 hold 窗口(配网页的配对码必须看得见)。
void pet_screen_set_user_lock(pet_screen_t *screen, bool lock, int64_t now_us);

bool pet_screen_is_locked(const pet_screen_t *screen);

// 到点查一次。idle_timeout_us <= 0 表示不自动息屏。
// 返回 true 表示状态**刚刚**变成息屏 —— 亮起来是 touch()/set_hold() 当场做的,
// 所以这里只会报"熄"这一种变化。
bool pet_screen_update(pet_screen_t *screen, int64_t now_us,
                       int64_t idle_timeout_us);

bool pet_screen_is_on(const pet_screen_t *screen);

// 当前该下发的背光百分比。持有期(必须看得见的画面)一律全亮; 否则链路睡眠时用
// 低亮度档 —— 宠物本体这时压暗到 40% 且帧速放慢, 屏幕跟着暗下来才是一致的表现。
uint8_t pet_screen_backlight(const pet_screen_t *screen, bool link_offline);
