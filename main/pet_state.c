// main/pet_state.c
#include "pet_state.h"

#include <string.h>

// 链接 / 状态 -> 动画行的基础映射(不含一次性过场动画)。
static pet_anim_t base_anim(pet_link_state_t link, pet_codex_state_t codex)
{
    switch (link) {
    case PET_LINK_OFFLINE:
        // 断线一律回到待机呼吸, 由 asleep 标记把它放慢并压暗成睡眠。
        return PET_ANIM_IDLE;
    case PET_LINK_CONNECTING:
        return PET_ANIM_WAITING;
    case PET_LINK_ONLINE:
        switch (codex) {
        case PET_CODEX_WORKING:
            return PET_ANIM_RUNNING;
        case PET_CODEX_WAITING:
            return PET_ANIM_WAITING;
        case PET_CODEX_READY:
            return PET_ANIM_REVIEW;
        case PET_CODEX_FAILED:
            return PET_ANIM_FAILED;
        case PET_CODEX_IDLE:
        default:
            return PET_ANIM_IDLE;
        }
    default:
        return PET_ANIM_IDLE;
    }
}

static void apply_anim(pet_state_t *state, pet_anim_t anim, bool once,
                       pet_anim_t next)
{
    state->anim = anim;
    state->once = once;
    state->next = next;
    state->transition = true;
}

void pet_state_init(pet_state_t *state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->link = PET_LINK_OFFLINE;
    state->codex = PET_CODEX_IDLE;
    apply_anim(state, PET_ANIM_IDLE, false, PET_ANIM_IDLE);
}

bool pet_state_set_link(pet_state_t *state, pet_link_state_t link)
{
    if (state == NULL || link >= PET_LINK_COUNT) return false;
    if (state->link == link) return false;

    const pet_link_state_t previous = state->link;
    state->link = link;

    // 刚连上时先挥手打招呼, 再落到该状态对应的循环动画。
    if (link == PET_LINK_ONLINE && previous != PET_LINK_ONLINE) {
        apply_anim(state, PET_ANIM_WAVING, true, base_anim(link, state->codex));
        return true;
    }

    apply_anim(state, base_anim(link, state->codex), false, PET_ANIM_IDLE);
    return true;
}

bool pet_state_set_codex(pet_state_t *state, pet_codex_state_t codex)
{
    if (state == NULL || codex >= PET_CODEX_COUNT) return false;
    if (state->codex == codex) return false;
    state->codex = codex;

    // 未连线时动画由链路状态决定, Codex 状态只记录下来等连线后再生效。
    if (state->link != PET_LINK_ONLINE) return false;

    const pet_anim_t desired = base_anim(state->link, codex);
    if (desired == state->anim && !state->once) return false;

    // 任务完成时先跳一下庆祝, 再进入专注评审的循环动画。
    if (desired == PET_ANIM_REVIEW) {
        apply_anim(state, PET_ANIM_JUMPING, true, PET_ANIM_REVIEW);
        return true;
    }

    apply_anim(state, desired, false, PET_ANIM_IDLE);
    return true;
}

bool pet_state_oneshot_complete(pet_state_t *state)
{
    if (state == NULL || !state->once) return false;
    const pet_anim_t next = state->next;
    apply_anim(state, next, false, PET_ANIM_IDLE);
    return true;
}

pet_pose_t pet_state_pose(const pet_state_t *state)
{
    pet_pose_t pose;
    if (state == NULL) {
        pose.anim = PET_ANIM_IDLE;
        pose.once = false;
        pose.next = PET_ANIM_IDLE;
        pose.asleep = true;
        pose.speed_pct = PET_STATE_SLEEP_SPEED_PCT;
        return pose;
    }

    pose.anim = state->anim;
    pose.once = state->once;
    pose.next = state->next;
    pose.asleep = state->link == PET_LINK_OFFLINE;
    pose.speed_pct = pose.asleep ? PET_STATE_SLEEP_SPEED_PCT : 100;
    return pose;
}

static const char *const CODEX_NAMES[PET_CODEX_COUNT] = {
    "idle", "working", "waiting", "ready", "failed",
};

const char *pet_codex_state_name(pet_codex_state_t state)
{
    if (state >= PET_CODEX_COUNT) return "idle";
    return CODEX_NAMES[state];
}

// 与 pet_atlas_sophie_portrait.c 里 PET_ATLAS_STATES 的 name 字段保持一致,
// 这样日志里看到的动作名和帧表里的行名能对上。
static const char *const ANIM_NAMES[PET_ANIM_COUNT] = {
    "idle", "running_right", "running_left", "waving", "jumping",
    "failed", "waiting", "running", "review",
};

const char *pet_anim_name(pet_anim_t anim)
{
    if (anim < 0 || anim >= PET_ANIM_COUNT) return "?";
    return ANIM_NAMES[anim];
}

bool pet_codex_state_parse(const char *name, pet_codex_state_t *out)
{
    if (name == NULL || out == NULL) return false;
    for (int i = 0; i < PET_CODEX_COUNT; i++) {
        if (strcmp(name, CODEX_NAMES[i]) == 0) {
            *out = (pet_codex_state_t)i;
            return true;
        }
    }
    return false;
}
