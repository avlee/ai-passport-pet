// main/pet_layout.h
// 宠物界面的版式计算。
//
// 为什么单独一个文件: 舞台尺寸现在是**运行时**的(每只宠物图集的裁剪区并集不同),
// 而屏幕只有 240x320、四角还被 BSP 切了 30 px 圆角, 所以"舞台摆在哪、台面接在哪
// 一行"必须算准。算错的后果是静默的 —— 宠物整体偏几像素, 没有任何东西会报错。
//
// 所以这里做成纯逻辑: 不依赖 ESP-IDF 与 LVGL, 主机测试可以逐条钉住
// (tests/test_pet_layout.c), 而 tools/preview_pet_screen.py 用同一批字面量出预览图。
//
// 约定: 本文件里的布局常量一律写成**整数字面量**。预览工具用正则直接读它们,
// 写成宏算式会被静默漏掉; 常量之间的等式由下面的 _Static_assert 在编译期兜住。
#pragma once

#include <stdbool.h>

// 屏幕。四角由 BSP 强制成 30 px 圆角, 所以靠边的元素都要躲开角落的弧线。
#define PET_LAYOUT_SCR_W 240
#define PET_LAYOUT_SCR_H 320

// 舞台顶边不许压到顶部状态行(y 6..28)。
#define PET_LAYOUT_STAGE_TOP_MIN 30
// 舞台左右至少要留出这么多, 否则会被 30 px 圆角切到。
#define PET_LAYOUT_STAGE_W_MAX 224
// 舞台至少要有这么大, 否则说明素材裁剪区是坏的。
#define PET_LAYOUT_STAGE_W_MIN 64
#define PET_LAYOUT_STAGE_H_MIN 64

// 槽里没有宠物时用的占位舞台尺寸。取的是参照宠物(sophie-portrait)的裁剪区并集,
// 这样"还没有宠物"的画面和装了参照宠物时的版式完全一致, 不会缺一块。
#define PET_LAYOUT_STAGE_W_REF 129
#define PET_LAYOUT_STAGE_H_REF 198

// 站台台面上沿, 也就是宠物脚底那一行(y 235)。台面以上的竖向区域属于舞台。
#define PET_LAYOUT_PLAT_FLOOR_Y 235
// 台面高度(y 235..249)。上下边框各被一条用量灯条占用, 灯条总高不得超过它。
#define PET_LAYOUT_PLAT_FLOOR_H 14

typedef struct {
    // 舞台(运行时按宠物包里的裁剪区并集算出来)
    int stage_x;
    int stage_y;
    int stage_w;
    int stage_h;

    // 站台: 台身(承载文本) + 台面 + 台面顶沿高光
    int plat_x;
    int plat_w;
    int plat_radius;
    int plat_floor_y;
    int plat_floor_h;
    int plat_floor_radius;
    int plat_rim_h;
    int plat_rim_inset;
    int plat_body_y;
    int plat_body_h;

    // 台身正面的文本块
    int plat_text_x;
    int plat_text_y;
    int plat_text_w;

    // 脚底接触阴影
    int shadow_y;
    int shadow_w;
    int shadow_h;

    // 睡眠标记 Zzz(相对舞台左上角)
    int sleep_x;
    int sleep_y;
} pet_layout_t;

// 屏幕上的一块矩形(左上角 + 宽高, 单位是像素)。
typedef struct {
    int x;
    int y;
    int w;
    int h;
} pet_layout_rect_t;

// 这个舞台尺寸画得下吗。画不下就拒绝安装这只宠物, 而不是画出一个越界的界面。
bool pet_layout_stage_supported(int stage_w, int stage_h);

// 计算版式。stage_* 不在支持范围内时返回 false, 且不写 *out。
bool pet_layout_for_stage(int stage_w, int stage_h, pet_layout_t *out);

// 把「一帧在图集单元格里的裁剪框」换算成「它在舞台里的落点」(相对舞台左上角)。
//
// 要减的基准是**舞台在图集单元格里的裁剪原点**(包头的 stage_x/stage_y, 也就是所有
// 帧裁剪框的并集原点, 见 pet_atlas_stage_x()), 不是舞台上屏幕的落点
// (pet_layout_t::stage_x/stage_y)。两个量在代码里都叫 stage_x, 却在两个不同的
// 坐标系里: 前者在单元格里(实测 31), 后者在屏幕上(实测 55)。混用不会编译失败、
// 不会越界崩溃, 只会让每帧整体偏移几十像素并被舞台裁掉一角 —— 第一次真机换宠物
// 就是这么错的。
//
// 所以换算只留这一个入口, 并且把不变量钉在这里: 帧的裁剪框必须含在舞台裁剪框里
// (pet_pkg_parse() 保证舞台就是所有帧裁剪框的并集, 所以对合法的包这一定成立),
// 落点也必须整个落在舞台矩形内。不成立就返回 false —— 宁可不画, 也不画一个错位的帧。
bool pet_layout_frame_rect(int frame_x, int frame_y, int frame_w, int frame_h,
                           int crop_x0, int crop_y0,
                           const pet_layout_t *layout, pet_layout_rect_t *out);
