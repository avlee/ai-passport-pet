// tests/dump_pet_layout.c
// 把 pet_layout_for_stage() 算出来的每个字段打出来, 供 Python 侧的镜像实现对照。
//
// 为什么需要这层: tools/preview_pet_screen.py 为了出预览图, 把同一套算术在 Python
// 里又写了一遍(它跑在编辑器里, 编译不了 C)。两份实现一旦跑偏, 表现是"预览看着居中、
// 真机偏几像素", 两边都不会报任何错 —— 预览的可信度全押在 tests/test_pet_layout_mirror.py
// 这条对照上。
//
// 用法:
//   dump_pet_layout <stage_w> <stage_h>
//     支持 -> 一行 key=value, 空格分隔
//     不支持 -> 一行 "unsupported"
//   dump_pet_layout <stage_w> <stage_h> <fx> <fy> <fw> <fh> <cx0> <cy0>
//     问"这一帧落在舞台里的哪儿"(裁剪框 + 舞台裁剪原点 -> 舞台内落点)
//     放得下 -> 一行 sprite_x=/sprite_y=/sprite_w=/sprite_h=
//     放不下 -> 一行 "rejected"(舞台本身不支持时是 "unsupported")
//
// 第二种模式与预览工具直接对应: 它出图时做的正是"减舞台裁剪原点再贴到舞台上",
// 两边一旦有一边减错了基准, 表现就是"预览居中、真机偏左上", 谁都不会报错。
//
// This runs on the host and needs no ESP-IDF.
#include <stdio.h>
#include <stdlib.h>

#include "pet_layout.h"

static void dump_layout(int stage_w, int stage_h)
{
    pet_layout_t layout;

    if (!pet_layout_for_stage(stage_w, stage_h, &layout)) {
        printf("unsupported\n");
        return;
    }

    printf("stage_x=%d stage_y=%d stage_w=%d stage_h=%d "
           "plat_x=%d plat_w=%d plat_radius=%d "
           "plat_floor_y=%d plat_floor_h=%d plat_floor_radius=%d "
           "plat_rim_h=%d plat_rim_inset=%d "
           "plat_body_y=%d plat_body_h=%d "
           "plat_text_x=%d plat_text_y=%d plat_text_w=%d "
           "shadow_y=%d shadow_w=%d shadow_h=%d "
           "sleep_x=%d sleep_y=%d\n",
           layout.stage_x, layout.stage_y, layout.stage_w, layout.stage_h,
           layout.plat_x, layout.plat_w, layout.plat_radius,
           layout.plat_floor_y, layout.plat_floor_h, layout.plat_floor_radius,
           layout.plat_rim_h, layout.plat_rim_inset,
           layout.plat_body_y, layout.plat_body_h,
           layout.plat_text_x, layout.plat_text_y, layout.plat_text_w,
           layout.shadow_y, layout.shadow_w, layout.shadow_h,
           layout.sleep_x, layout.sleep_y);
}

static void dump_frame_rect(int stage_w, int stage_h, int fx, int fy, int fw,
                            int fh, int cx0, int cy0)
{
    pet_layout_t layout;
    pet_layout_rect_t rect;

    if (!pet_layout_for_stage(stage_w, stage_h, &layout)) {
        printf("unsupported\n");
        return;
    }
    if (!pet_layout_frame_rect(fx, fy, fw, fh, cx0, cy0, &layout, &rect)) {
        printf("rejected\n");
        return;
    }

    printf("sprite_x=%d sprite_y=%d sprite_w=%d sprite_h=%d\n",
           rect.x, rect.y, rect.w, rect.h);
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        dump_layout(atoi(argv[1]), atoi(argv[2]));
        return 0;
    }
    if (argc == 9) {
        dump_frame_rect(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]),
                        atoi(argv[4]), atoi(argv[5]), atoi(argv[6]),
                        atoi(argv[7]), atoi(argv[8]));
        return 0;
    }

    fprintf(stderr,
            "usage: %s STAGE_W STAGE_H [FRAME_X FRAME_Y FRAME_W FRAME_H "
            "CROP_X0 CROP_Y0]\n", argv[0]);
    return 2;
}
