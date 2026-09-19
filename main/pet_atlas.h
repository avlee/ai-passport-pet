// main/pet_atlas.h
// 宠物图集访问层: 把生成出来的帧表包装成 LVGL 可以直接绘制的图像描述符。
//
// 生成的帧表(主文件 main/pet_atlas_<pet>.h + .c)由 tools/gen_pet_assets.py
// 从 v2 图集产出; 帧表用 extern 声明, 所以本头文件可以被多个编译单元安全包含。
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "pet_atlas_sophie_portrait.h"  // 生成文件: 帧表 + PET_ATLAS_STAGE_*
#include "pet_state.h"

// 取某个动画行的帧数。anim 越界返回 0。
uint16_t pet_atlas_frame_count(pet_anim_t anim);

// 取某一帧在 192x208 单元格内的裁剪信息。越界返回 NULL。
const pet_atlas_frame_t *pet_atlas_frame(pet_anim_t anim, uint16_t index);

// 取可直接传给 lv_image_set_src() 的图像描述符。越界返回 NULL。
const lv_image_dsc_t *pet_atlas_image(pet_anim_t anim, uint16_t index);

// 构建全部帧描述符并做一次一致性自检。必须在 bsp_lvgl_init() 之后调用一次。
void pet_atlas_init(void);

// 图集所有动画行里出现过的最大裁剪区域(相对单元格原点),
// 也就是屏幕上那块固定大小的绘制外框。
uint16_t pet_atlas_stage_width(void);
uint16_t pet_atlas_stage_height(void);
