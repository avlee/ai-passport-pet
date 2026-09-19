// Host test for the Codex pet state machine (no ESP-IDF/LVGL needed).
#include <assert.h>
#include <string.h>

#include "pet_state.h"

static void test_offline_is_asleep(void)
{
    pet_state_t state;
    pet_state_init(&state);

    pet_pose_t pose = pet_state_pose(&state);
    assert(pose.anim == PET_ANIM_IDLE);
    assert(pose.asleep);
    assert(pose.speed_pct == PET_STATE_SLEEP_SPEED_PCT);
}

static void test_connect_waves_then_returns(void)
{
    pet_state_t state;
    pet_state_init(&state);

    // CONNECTING 显示等待动画。
    assert(pet_state_set_link(&state, PET_LINK_CONNECTING));
    assert(pet_state_pose(&state).anim == PET_ANIM_WAITING);

    // 连上先挥手, 而且是一次性播放。
    assert(pet_state_set_link(&state, PET_LINK_ONLINE));
    pet_pose_t pose = pet_state_pose(&state);
    assert(pose.anim == PET_ANIM_WAVING);
    assert(pose.once);
    assert(!pose.asleep);

    // 播完一轮后回到 idle。
    assert(pet_state_oneshot_complete(&state));
    pose = pet_state_pose(&state);
    assert(pose.anim == PET_ANIM_IDLE);
    assert(!pose.once);

    // 重复置为同一链路状态不应触发变化。
    assert(!pet_state_set_link(&state, PET_LINK_ONLINE));
}

static void test_codex_state_mapping(void)
{
    pet_state_t state;
    pet_state_init(&state);
    pet_state_set_link(&state, PET_LINK_ONLINE);
    pet_state_oneshot_complete(&state);

    assert(pet_state_set_codex(&state, PET_CODEX_WORKING));
    assert(pet_state_pose(&state).anim == PET_ANIM_RUNNING);

    assert(pet_state_set_codex(&state, PET_CODEX_WAITING));
    assert(pet_state_pose(&state).anim == PET_ANIM_WAITING);

    assert(pet_state_set_codex(&state, PET_CODEX_FAILED));
    assert(pet_state_pose(&state).anim == PET_ANIM_FAILED);

    // 未变化的状态不应报告过渡。
    assert(!pet_state_set_codex(&state, PET_CODEX_FAILED));
}

static void test_ready_jumps_then_reviews(void)
{
    pet_state_t state;
    pet_state_init(&state);
    pet_state_set_link(&state, PET_LINK_ONLINE);
    pet_state_oneshot_complete(&state);

    pet_state_set_codex(&state, PET_CODEX_WORKING);
    assert(pet_state_set_codex(&state, PET_CODEX_READY));

    pet_pose_t pose = pet_state_pose(&state);
    assert(pose.anim == PET_ANIM_JUMPING);
    assert(pose.once);
    assert(pose.next == PET_ANIM_REVIEW);

    assert(pet_state_oneshot_complete(&state));
    pose = pet_state_pose(&state);
    assert(pose.anim == PET_ANIM_REVIEW);
    assert(!pose.once);
}

static void test_state_change_interrupts_oneshot(void)
{
    pet_state_t state;
    pet_state_init(&state);
    pet_state_set_link(&state, PET_LINK_ONLINE);

    // 挥手还没播完, 任务已经开始 —— 必须立刻切到工作动画。
    assert(pet_state_pose(&state).anim == PET_ANIM_WAVING);
    assert(pet_state_set_codex(&state, PET_CODEX_WORKING));
    pet_pose_t pose = pet_state_pose(&state);
    assert(pose.anim == PET_ANIM_RUNNING);
    assert(!pose.once);
}

static void test_disconnect_returns_to_sleep(void)
{
    pet_state_t state;
    pet_state_init(&state);
    pet_state_set_link(&state, PET_LINK_ONLINE);
    pet_state_oneshot_complete(&state);
    pet_state_set_codex(&state, PET_CODEX_WORKING);
    assert(pet_state_pose(&state).anim == PET_ANIM_RUNNING);

    assert(pet_state_set_link(&state, PET_LINK_OFFLINE));
    pet_pose_t pose = pet_state_pose(&state);
    assert(pose.anim == PET_ANIM_IDLE);
    assert(pose.asleep);
    assert(pose.speed_pct == PET_STATE_SLEEP_SPEED_PCT);

    // 断线期间 Codex 状态只记录, 不改变动画。
    assert(!pet_state_set_codex(&state, PET_CODEX_READY));
    assert(pet_state_pose(&state).anim == PET_ANIM_IDLE);

    // 重新连上先挥手, 然后落到断线期间记录下来的状态。
    assert(pet_state_set_link(&state, PET_LINK_ONLINE));
    assert(pet_state_pose(&state).anim == PET_ANIM_WAVING);
    assert(pet_state_pose(&state).next == PET_ANIM_REVIEW);
}

static void test_state_names_round_trip(void)
{
    for (int i = 0; i < PET_CODEX_COUNT; i++) {
        pet_codex_state_t parsed = PET_CODEX_COUNT;
        assert(pet_codex_state_parse(pet_codex_state_name((pet_codex_state_t)i),
                                     &parsed));
        assert(parsed == (pet_codex_state_t)i);
    }

    pet_codex_state_t ignored = PET_CODEX_WORKING;
    assert(!pet_codex_state_parse("busy", &ignored));
    assert(ignored == PET_CODEX_WORKING);
    assert(!pet_codex_state_parse(NULL, &ignored));
    assert(strcmp(pet_codex_state_name(PET_CODEX_COUNT), "idle") == 0);
}

static void test_invalid_input_is_ignored(void)
{
    pet_state_t state;
    pet_state_init(&state);
    assert(!pet_state_set_link(&state, PET_LINK_COUNT));
    assert(!pet_state_set_codex(&state, PET_CODEX_COUNT));
    assert(!pet_state_oneshot_complete(&state));
    assert(pet_state_pose(NULL).asleep);
}

int main(void)
{
    test_offline_is_asleep();
    test_connect_waves_then_returns();
    test_codex_state_mapping();
    test_ready_jumps_then_reviews();
    test_state_change_interrupts_oneshot();
    test_disconnect_returns_to_sleep();
    test_state_names_round_trip();
    test_invalid_input_is_ignored();
    return 0;
}
