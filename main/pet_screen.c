// main/pet_screen.c
#include "pet_screen.h"

#include "pet_config.h"

void pet_screen_init(pet_screen_t *screen, int64_t now_us)
{
    screen->state = PET_SCREEN_ON;
    screen->last_activity_us = now_us;
    screen->hold = false;
    screen->user_lock = false;
}

void pet_screen_touch(pet_screen_t *screen, int64_t now_us)
{
    // 锁定期 touch 是空操作: 消息、链路恢复都不许把屏点亮, 这是锁定的本意。
    if (screen->user_lock) return;
    screen->last_activity_us = now_us;
    screen->state = PET_SCREEN_ON;
}

void pet_screen_set_hold(pet_screen_t *screen, bool hold, int64_t now_us)
{
    screen->hold = hold;
    screen->last_activity_us = now_us;
    // 锁定期 hold 只记账不亮屏。调用侧的正常流程不会在锁定时进 hold 窗口
    // (进窗口靠按键, 按键先解锁), 这里是防御。
    if (hold && !screen->user_lock) screen->state = PET_SCREEN_ON;
}

void pet_screen_set_user_lock(pet_screen_t *screen, bool lock, int64_t now_us)
{
    if (screen->user_lock == lock) return;
    screen->user_lock = lock;
    if (lock) {
        screen->state = PET_SCREEN_OFF;
    } else {
        // 解锁即 touch: 亮屏, 并把空闲计时从这一刻重算 —— 否则锁了很久之后
        // 一解锁就被 update() 当场判成"超时", 看一眼就黑。
        screen->last_activity_us = now_us;
        screen->state = PET_SCREEN_ON;
    }
}

bool pet_screen_is_locked(const pet_screen_t *screen)
{
    return screen->user_lock;
}

bool pet_screen_update(pet_screen_t *screen, int64_t now_us,
                       int64_t idle_timeout_us)
{
    if (screen->state != PET_SCREEN_ON) return false;
    if (screen->user_lock) return false;
    if (screen->hold) return false;
    if (idle_timeout_us <= 0) return false;
    if (now_us - screen->last_activity_us < idle_timeout_us) return false;

    screen->state = PET_SCREEN_OFF;
    return true;
}

bool pet_screen_is_on(const pet_screen_t *screen)
{
    return screen->state == PET_SCREEN_ON;
}

uint8_t pet_screen_backlight(const pet_screen_t *screen, bool link_offline)
{
    // 息屏就是背光归零。0 不是"再暗一点": 它把背光这一路的电流整个切掉; 面板显存
    // 不掉, 所以亮回来是瞬时的, 不需要重绘整屏。
    if (screen->state != PET_SCREEN_ON) return 0;

    // 持有期内一律全亮。配网页就是最典型的例子: 它出现的时候设备**本来就还没联
    // 网**, 拿链路状态去定亮度会把配对码显示在 45% 的暗屏上; 而这几块画面(配对码、
    // 传输进度)正是要求用户看清楚的那种。
    if (screen->hold) return PET_ACTIVE_BACKLIGHT_PCT;

    return link_offline ? PET_SLEEP_BACKLIGHT_PCT : PET_ACTIVE_BACKLIGHT_PCT;
}
