// main/pet_atlas.h
// 宠物图集访问层: 把宠物槽里那份包的帧表包装成 LVGL 可以直接绘制的图像描述符。
//
// 与从前最大的区别是**宠物不再是编译期常量**。以前 main/CMakeLists.txt 用
// EMBED_FILES 把 blob 链接进 app 分区, 帧表是生成的 C 数组; 现在包躺在
// partitions.csv 的 `pets` 分区里, 由 pet_slot 映射进来, 这里只是给它套一层
// lv_image_dsc_t。
//
// 零拷贝: 描述符里的 data 指针直接指进 mmap 出来的只读 flash, 每帧像素不复制、
// 不占 RAM。所以装载一次之后切换动画帧是纯粹的指针移动。
//
// 生命周期: 写宠物槽之前必须先 pet_atlas_unload() —— 一边 mmap 一边擦写同一块
// flash 是未定义行为。界面上也不能再有对象引用旧的描述符, 所以卸载总是和
// pet_ui_destroy() 配对出现。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "pet_pkg.h"
#include "pet_state.h"

// 从宠物槽装载图集。失败(槽是空的 / 包坏了 / 映射不出来)时返回 false, 界面应当
// 显示"还没有宠物", 而不是画一堆错位的帧。
bool pet_atlas_load(void);

// 放弃映射与全部帧描述符。可重复调用。
void pet_atlas_unload(void);

bool pet_atlas_ready(void);

// 当前宠物的 id; 没有宠物时返回 PET_SLOT_NO_PET。hello 报文用它上报身份。
const char *pet_atlas_pet_id(void);

// 当前宠物的展示名(包里的 display_name)。没有宠物时返回空串。
const char *pet_atlas_display_name(void);

// 取某个动画行的帧数。没有宠物或 anim 越界返回 0。
uint16_t pet_atlas_frame_count(pet_anim_t anim);

// 取某一帧的裁剪信息(相对 192x208 单元格)。越界返回 NULL。
const pet_pkg_frame_t *pet_atlas_frame(pet_anim_t anim, uint16_t index);

// 取可直接传给 lv_image_set_src() 的图像描述符。越界返回 NULL。
const lv_image_dsc_t *pet_atlas_image(pet_anim_t anim, uint16_t index);

// 所有帧裁剪区的并集 —— 屏幕上那块固定大小的绘制外框, 也是每帧裁剪原点要减掉的
// 基准点。没有宠物时全部返回 0。
uint16_t pet_atlas_stage_x(void);
uint16_t pet_atlas_stage_y(void);
uint16_t pet_atlas_stage_width(void);
uint16_t pet_atlas_stage_height(void);
