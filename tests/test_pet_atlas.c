// Host test for the generated pet atlas tables.
//
// 这是「换图集后动画还能对上」的守门测试: 它不看任何一个像素, 只验证生成脚本
// 产出的帧表本身自洽 —— 行顺序、帧连续性、裁剪范围、blob 边界与对齐。
#include <assert.h>
#include <stdio.h>

#include "pet_atlas_sophie_portrait.h"
#include "pet_atlas_validate.h"
#include "pet_state.h"

int main(void)
{
    // 完整校验必须通过(返回码含义见 pet_atlas_validate.c)。
    const uint32_t error = pet_atlas_validate(PET_ATLAS_BLOB_SIZE);
    if (error != 0) {
        printf("pet_atlas_validate failed with code %u\n", (unsigned)error);
    }
    assert(error == 0);

    // 动画行数必须与状态机枚举一一对应。
    assert(PET_ATLAS_STATE_COUNT == PET_ANIM_COUNT);
    assert(PET_ATLAS_FRAME_COUNT == 57);

    // 第 0 行是 idle, 前 6 帧; 顺序与 pet_anim_t 一致。
    assert(PET_ATLAS_STATES[PET_ANIM_IDLE].first == 0);
    assert(PET_ATLAS_STATES[PET_ANIM_IDLE].count == 6);
    assert(PET_ATLAS_STATES[PET_ANIM_REVIEW].first == 51);
    assert(PET_ATLAS_STATES[PET_ANIM_REVIEW].count == 6);

    // idle 第一帧就是「减少动效」静态姿势: 必须是时长最长的那一帧之外的首帧,
    // 且宽度明显小于整格 —— 说明裁剪确实生效了(否则 flash 会白占 3 倍)。
    const pet_atlas_frame_t *first = &PET_ATLAS_STATES[PET_ANIM_IDLE].frames[0];
    assert(first->w < PET_ATLAS_CELL_W);
    assert(first->h < PET_ATLAS_CELL_H);
    assert(first->offset == 0);

    // 第 2 帧起必须 4 字节对齐。
    for (int s = 0; s < PET_ATLAS_STATE_COUNT; s++) {
        for (uint16_t i = 0; i < PET_ATLAS_STATES[s].count; i++) {
            assert((PET_ATLAS_STATES[s].frames[i].offset % 4) == 0);
        }
    }

    // 绘制外框必须覆盖所有帧的裁剪区域, 否则动画会被裁掉一部分。
    for (int s = 0; s < PET_ATLAS_STATE_COUNT; s++) {
        for (uint16_t i = 0; i < PET_ATLAS_STATES[s].count; i++) {
            const pet_atlas_frame_t *frame = &PET_ATLAS_STATES[s].frames[i];
            assert(frame->x >= PET_ATLAS_STAGE_X);
            assert(frame->y >= PET_ATLAS_STAGE_Y);
            assert(frame->x + frame->w <= PET_ATLAS_STAGE_X + PET_ATLAS_STAGE_W);
            assert(frame->y + frame->h <= PET_ATLAS_STAGE_Y + PET_ATLAS_STAGE_H);
        }
    }

    // 负例: 尺寸缩小 1 字节后越界检查必须报错, 证明校验不是恒真。
    assert(pet_atlas_validate(PET_ATLAS_BLOB_SIZE - 1) != 0);

    return 0;
}
