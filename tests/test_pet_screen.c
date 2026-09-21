// Host test for the screen power policy (no ESP-IDF/LVGL needed).
#include <assert.h>

#include "pet_config.h"
#include "pet_screen.h"

// 判定用的窗口就取 pet_config.h 里那个值 —— 测试钉的是边界, 不是某个具体秒数。
#define IDLE_US ((int64_t)PET_SCREEN_IDLE_MS * 1000)

static void test_boot_is_lit_and_goes_dark_at_the_deadline(void)
{
    pet_screen_t screen;
    pet_screen_init(&screen, 0);

    assert(pet_screen_is_on(&screen));
    // 还差 1 µs: 不许提前黑。
    assert(!pet_screen_update(&screen, IDLE_US - 1, IDLE_US));
    assert(pet_screen_is_on(&screen));

    // 正好到点就黑。
    assert(pet_screen_update(&screen, IDLE_US, IDLE_US));
    assert(!pet_screen_is_on(&screen));

    // 已经黑着: 再查多少次都不该报"又变了一次", 否则调用方会一遍遍重写背光。
    assert(!pet_screen_update(&screen, IDLE_US * 10, IDLE_US));
}

static void test_activity_restarts_the_clock(void)
{
    pet_screen_t screen;
    pet_screen_init(&screen, 0);

    const int64_t touched = IDLE_US - 1;
    pet_screen_touch(&screen, touched);

    assert(!pet_screen_update(&screen, touched + IDLE_US - 1, IDLE_US));
    assert(pet_screen_update(&screen, touched + IDLE_US, IDLE_US));
}

static void test_touch_lights_a_dark_screen_again(void)
{
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    assert(pet_screen_update(&screen, IDLE_US, IDLE_US));
    assert(pet_screen_backlight(&screen, false) == 0);

    pet_screen_touch(&screen, IDLE_US + 5);
    assert(pet_screen_is_on(&screen));
    assert(pet_screen_backlight(&screen, false) == PET_ACTIVE_BACKLIGHT_PCT);
}

static void test_hold_keeps_the_screen_lit(void)
{
    pet_screen_t screen;
    pet_screen_init(&screen, 0);

    // 配网页 / 宠物接收页开着: 无论过多久都不许黑。
    pet_screen_set_hold(&screen, true, 0);
    assert(pet_screen_is_on(&screen));
    assert(!pet_screen_update(&screen, IDLE_US * 30, IDLE_US));
    assert(pet_screen_is_on(&screen));
}

static void test_hold_lights_a_dark_screen(void)
{
    // 屏幕已经黑了, 这时 Mac 推来一只宠物 —— 进度必须立刻看得见。
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    assert(pet_screen_update(&screen, IDLE_US, IDLE_US));

    pet_screen_set_hold(&screen, true, IDLE_US + 1);
    assert(pet_screen_is_on(&screen));
    assert(pet_screen_backlight(&screen, false) == PET_ACTIVE_BACKLIGHT_PCT);
}

static void test_hold_is_always_full_brightness(void)
{
    // 配网页出现时设备本来就还没联网。要是拿链路状态去定亮度, 配对码会显示在
    // 45% 的暗屏上 —— 而"必须看得见"的画面正是要求用户看清楚的那种。
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    pet_screen_set_hold(&screen, true, 0);

    assert(pet_screen_backlight(&screen, true) == PET_ACTIVE_BACKLIGHT_PCT);
    assert(pet_screen_backlight(&screen, false) == PET_ACTIVE_BACKLIGHT_PCT);
}

static void test_releasing_hold_restarts_the_clock(void)
{
    // 传输结束后屏幕上还有一句话(成功或失败)要给人看。如果拿 hold 期间那个旧的空闲
    // 时刻继续算, 那句话会在 0 秒内被关掉 —— 现象是"屏幕上闪了一下就没了"。
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    pet_screen_set_hold(&screen, true, 0);

    const int64_t released = IDLE_US * 30;
    pet_screen_set_hold(&screen, false, released);
    assert(pet_screen_is_on(&screen));
    assert(!pet_screen_update(&screen, released + IDLE_US - 1, IDLE_US));
    assert(pet_screen_update(&screen, released + IDLE_US, IDLE_US));
}

static void test_backlight_follows_link_state(void)
{
    pet_screen_t screen;
    pet_screen_init(&screen, 0);

    // 亮屏: 在线全亮; 链路睡眠时用低亮度档 —— 宠物这时压暗到 40% 且放慢, 屏幕
    // 跟着暗下来才是一致的表现。
    assert(pet_screen_backlight(&screen, false) == PET_ACTIVE_BACKLIGHT_PCT);
    assert(pet_screen_backlight(&screen, true) == PET_SLEEP_BACKLIGHT_PCT);

    // 息屏: 不管链路什么状态都必须归零, 否则"息屏"就只是"暗一点"。
    assert(pet_screen_update(&screen, IDLE_US, IDLE_US));
    assert(pet_screen_backlight(&screen, false) == 0);
    assert(pet_screen_backlight(&screen, true) == 0);
}

static void test_zero_timeout_disables_the_auto_off(void)
{
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    assert(!pet_screen_update(&screen, IDLE_US * 1000, 0));
    assert(pet_screen_is_on(&screen));
}

static void test_touch_and_hold_keep_the_clock_monotonic(void)
{
    // 活动时间只前进: 拿到一个更早的时刻(理论上不该发生)也不该把窗口算短或算长,
    // 只要求它不崩、状态仍然自洽。
    pet_screen_t screen;
    pet_screen_init(&screen, 100);
    pet_screen_touch(&screen, 50);
    assert(screen.last_activity_us == 50);
    assert(pet_screen_update(&screen, 50 + IDLE_US, IDLE_US));
}

static void test_user_lock_darkens_and_ignores_activity(void)
{
    // 双击确定息屏: 锁定立刻黑, 之后消息/链路恢复(touch)再吵也不亮。
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    assert(pet_screen_is_locked(&screen) == false);

    pet_screen_set_user_lock(&screen, true, 1);
    assert(pet_screen_is_locked(&screen));
    assert(!pet_screen_is_on(&screen));
    assert(pet_screen_backlight(&screen, false) == 0);

    pet_screen_touch(&screen, 2);
    assert(!pet_screen_is_on(&screen));

    // 锁定期间的空闲超时也不会"再变一次"(本来就黑着), 背光不会出错。
    assert(!pet_screen_update(&screen, IDLE_US * 10, IDLE_US));
    assert(pet_screen_backlight(&screen, true) == 0);
}

static void test_user_lock_mutes_hold_but_uninterrupted_keys_unlock(void)
{
    // 锁定后 hold 只记账不亮屏(防御: 正常流程按键先解锁); 解锁亮屏并重置
    // 空闲计时 —— 否则锁了很久之后一解锁就被 update() 当场判成超时。
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    pet_screen_set_user_lock(&screen, true, 1);

    pet_screen_set_hold(&screen, true, 2);
    assert(!pet_screen_is_on(&screen));

    const int64_t unlock_at = IDLE_US * 100;
    pet_screen_set_user_lock(&screen, false, unlock_at);
    assert(!pet_screen_is_locked(&screen));
    assert(pet_screen_is_on(&screen));

    // 解锁把空闲计时从这一刻重算; 释放 hold 也一样 —— 计时基准不许再回到
    // 锁定期/hold 期的旧时刻, 否则一解锁就"看一眼就黑"。
    pet_screen_set_hold(&screen, false, unlock_at + 1);
    assert(pet_screen_is_on(&screen));
    assert(!pet_screen_update(&screen, unlock_at + 1 + IDLE_US - 1, IDLE_US));
    assert(pet_screen_update(&screen, unlock_at + 1 + IDLE_US, IDLE_US));
}

static void test_relocking_and_unlocking_is_idempotent(void)
{
    // 重复设同一个状态不该有任何副作用(比如把空闲计时抹掉)。
    pet_screen_t screen;
    pet_screen_init(&screen, 0);
    pet_screen_set_user_lock(&screen, true, 1);
    pet_screen_set_user_lock(&screen, true, 2);
    assert(!pet_screen_is_on(&screen));

    pet_screen_set_user_lock(&screen, false, 10);
    pet_screen_set_user_lock(&screen, false, 20);
    assert(pet_screen_is_on(&screen));
    assert(screen.last_activity_us == 10);
}

int main(void)
{
    test_boot_is_lit_and_goes_dark_at_the_deadline();
    test_activity_restarts_the_clock();
    test_touch_lights_a_dark_screen_again();
    test_hold_keeps_the_screen_lit();
    test_hold_lights_a_dark_screen();
    test_hold_is_always_full_brightness();
    test_releasing_hold_restarts_the_clock();
    test_backlight_follows_link_state();
    test_zero_timeout_disables_the_auto_off();
    test_touch_and_hold_keep_the_clock_monotonic();
    test_user_lock_darkens_and_ignores_activity();
    test_user_lock_mutes_hold_but_uninterrupted_keys_unlock();
    test_relocking_and_unlocking_is_idempotent();
    return 0;
}
