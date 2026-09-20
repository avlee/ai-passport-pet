// main/pet_screen.c
#include "pet_screen.h"

#include "pet_config.h"

void pet_screen_init(pet_screen_t *screen, int64_t now_us)
{
    screen->state = PET_SCREEN_ON;
    screen->last_activity_us = now_us;
    screen->hold = false;
}

void pet_screen_touch(pet_screen_t *screen, int64_t now_us)
{
    screen->last_activity_us = now_us;
    screen->state = PET_SCREEN_ON;
}

void pet_screen_set_hold(pet_screen_t *screen, bool hold, int64_t now_us)
{
    screen->hold = hold;
    screen->last_activity_us = now_us;
    if (hold) screen->state = PET_SCREEN_ON;
}

bool pet_screen_update(pet_screen_t *screen, int64_t now_us,
                       int64_t idle_timeout_us)
{
    if (screen->state != PET_SCREEN_ON) return false;
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
