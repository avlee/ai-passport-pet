// main/pet_atlas_validate.c
#include "pet_atlas_validate.h"

#include <string.h>

#include "pet_atlas_sophie_portrait.h"
#include "pet_state.h"

#define PET_ATLAS_BYTES_PER_PIXEL 3
#define PET_ATLAS_MIN_FRAMES_PER_STATE 2

// 生成脚本按 pet_anim_t 的顺序输出动画行。顺序一旦错位, 动画就会跳到别的
// 动作上, 而且不会崩溃 —— 所以必须显式校验名字。
static const char *const EXPECTED_STATE_NAMES[PET_ANIM_COUNT] = {
    "idle",        "running_right", "running_left", "waving", "jumping",
    "failed",      "waiting",       "running",      "review",
};

uint32_t pet_atlas_validate(uint32_t blob_size)
{
    // 1: 生成的动画行数与 pet_anim_t 不一致(生成脚本与头文件不同步)
    if (PET_ATLAS_STATE_COUNT != PET_ANIM_COUNT) return 1;

    uint32_t next_expected_first = 0;

    for (int s = 0; s < PET_ATLAS_STATE_COUNT; s++) {
        const pet_atlas_state_t *state = &PET_ATLAS_STATES[s];

        // 2: 动画行名字与 pet_anim_t 的顺序不符
        if (state->name == NULL ||
            strcmp(state->name, EXPECTED_STATE_NAMES[s]) != 0) {
            return 2;
        }
        // 3: 帧数太少(单帧动画无法体现动作)
        if (state->count < PET_ATLAS_MIN_FRAMES_PER_STATE) return 3;
        // 4: 帧表不连续(生成脚本漏写或多写)
        if (state->first != next_expected_first) return 4;

        for (uint16_t i = 0; i < state->count; i++) {
            const pet_atlas_frame_t *frame = &state->frames[i];
            const uint32_t size = (uint32_t)frame->w * (uint32_t)frame->h *
                                  PET_ATLAS_BYTES_PER_PIXEL;

            // 5: 宽高非法
            if (frame->w <= 0 || frame->h <= 0) return 5;
            // 6: 裁剪区域超出 192x208 单元格
            if (frame->x < 0 || frame->y < 0 ||
                frame->x + frame->w > PET_ATLAS_CELL_W ||
                frame->y + frame->h > PET_ATLAS_CELL_H) {
                return 6;
            }
            // 7: 帧时长缺失
            if (frame->duration == 0) return 7;
            // 8: 帧数据越出嵌入 blob
            if ((uint32_t)frame->offset + size > blob_size) return 8;
        }

        next_expected_first += state->count;
    }

    // 9: 行内帧数与总帧数不一致
    if (next_expected_first != PET_ATLAS_FRAME_COUNT) return 9;

    // 10: 帧数据在 blob 中必须严格递增且不重叠(生成器每帧 4 字节对齐)。
    uint32_t previous_end = 0;
    for (int s = 0; s < PET_ATLAS_STATE_COUNT; s++) {
        const pet_atlas_state_t *state = &PET_ATLAS_STATES[s];
        for (uint16_t i = 0; i < state->count; i++) {
            const pet_atlas_frame_t *frame = &state->frames[i];
            const uint32_t size = (uint32_t)frame->w * (uint32_t)frame->h *
                                  PET_ATLAS_BYTES_PER_PIXEL;
            if (frame->offset < previous_end) return 10;
            previous_end = (uint32_t)frame->offset + size;
        }
    }

    // 11: 每帧数据必须 4 字节对齐(draw 时按 RGB565 半字读取)。
    for (int s = 0; s < PET_ATLAS_STATE_COUNT; s++) {
        const pet_atlas_state_t *state = &PET_ATLAS_STATES[s];
        for (uint16_t i = 0; i < state->count; i++) {
            if ((state->frames[i].offset % 4) != 0) return 11;
        }
    }

    return 0;
}
