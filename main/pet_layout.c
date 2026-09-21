// main/pet_layout.c
#include "pet_layout.h"

#include <stddef.h>

// ---------------------------------------------------------------------------
// 参照宠物(sophie-portrait, 舞台 129x198)的那一套坐标。
//
// 舞台的**垂直**位置锚在脚底而不是顶边: 台面上沿永远落在宠物脚底那一行, 换宠物时
// 由 pet_layout_for_stage() 反推实际的 stage_y, 站台自己一动不动。这样无论宠物多高,
// 它都站在同一块台子上, 而不是"宠物动了、台子跟着跑"。
//
//   y   6 ..  28   状态圆点 + 状态文字(左) / 电量(右) —— 顶栏固定版式, 见 pet_ui.c
//   y  38 .. 235   宠物舞台 129x198, 不画底, 直接立在背景上
//   y 235 .. 314   站台: 台面 + 台身
// ---------------------------------------------------------------------------
// 顶栏(状态圆点/文字/电量)与舞台尺寸无关, 留在 pet_ui.c 里当字面量。

// 参照宠物的舞台: 129x198 的裁剪区并集, 水平居中(240 - 129 = 111 -> 左边距 55),
// 底行 235 就是宠物脚底。
#define STAGE_W_REF PET_LAYOUT_STAGE_W_REF
#define STAGE_H_REF PET_LAYOUT_STAGE_H_REF
#define STAGE_Y_REF  38

// 站台左右与屏幕中心对称: (240 - 212) / 2 = 14。
#define PLAT_X        14
#define PLAT_W       212
#define PLAT_RADIUS   14

// 台面上沿 = 宠物脚底那一行。真值定义在 pet_layout.h(PET_LAYOUT_PLAT_FLOOR_Y /
// PET_LAYOUT_PLAT_FLOOR_H), pet_ui.c 的灯条断言也引用它们, 不在这里重复写数字。
#define PLAT_FLOOR_Y PET_LAYOUT_PLAT_FLOOR_Y
#define PLAT_FLOOR_H PET_LAYOUT_PLAT_FLOOR_H
// 台面比台身矮得多, 用 PLAT_RADIUS 会鼓成胶囊, 单独给一个小圆角。
#define PLAT_FLOOR_RADIUS 5
#define PLAT_RIM_H     2
#define PLAT_RIM_INSET 6

// 台身到 314 为止: 屏幕底部 30 px 圆角在 x=14 处从 y≈314 开始收, 再往下会被切。
#define PLAT_BODY_Y  249
#define PLAT_BODY_H   65

// 脚底接触阴影: 压在台面正中, 让宠物"贴"在台面上而不是飘着。
// 别做太大 —— 台面只有 14 px 高, 阴影盖满就变成台面上一个黑洞。
#define SHADOW_Y     236
#define SHADOW_W      56
#define SHADOW_H       7

// 文本块: 居中放在台身正面, 两行高。PAD=5: 字库行高修正(31->22)后文字整体
// 贴台身顶, 视觉偏上; 队长定稿下移 3px。两行文本的字形底最深到 y=296,
// 5h/7d 标签行框顶 294 —— 水平上标签只占台身两角、文字居中, 实际字形
// 贴线不交叠; 若日后要再动这两个值, 一起看真机两行长文本。
#define PLAT_TEXT_PAD  5
#define PLAT_TEXT_W  184

// 睡眠标记浮在宠物右上方那块空白上(素材头部只占左半边), 相对舞台左上角。
#define SLEEP_X     98
#define SLEEP_Y      6

// ---------------------------------------------------------------------------
// 版式恒等式。全部是整数常量表达式, 在编译期求值。
// ---------------------------------------------------------------------------
_Static_assert(PLAT_FLOOR_Y == STAGE_Y_REF + STAGE_H_REF - 1,
               "台面上沿必须正好压在参照宠物的脚底那一行");
_Static_assert(PLAT_BODY_Y == PLAT_FLOOR_Y + PLAT_FLOOR_H,
               "台身必须紧接台面下沿");
_Static_assert(PLAT_X + PLAT_W == PET_LAYOUT_SCR_W - PLAT_X,
               "站台必须水平居中");
_Static_assert(PLAT_BODY_Y + PLAT_BODY_H <= PET_LAYOUT_SCR_H - 6,
               "站台底边要给屏幕底部的圆角和边框留出余量");
_Static_assert(SHADOW_Y >= PLAT_FLOOR_Y && SHADOW_Y + SHADOW_H <= PLAT_BODY_Y,
               "接触阴影必须整个落在台面上");
_Static_assert(PLAT_TEXT_W == PLAT_W - 28,
               "文本宽度 = 站台宽度减去左右各 14 px 内边距");
_Static_assert(STAGE_Y_REF == PLAT_FLOOR_Y + 1 - STAGE_H_REF,
               "参照舞台的顶边必须正好是脚底对齐反推出来的值");
_Static_assert(STAGE_Y_REF >= PET_LAYOUT_STAGE_TOP_MIN,
               "参照舞台的顶边不能压到顶部状态行");
// 上限藏在这里: 舞台再高一点, 顶边就会顶进状态行。pet_layout_stage_supported()
// 用同一个式子做运行时判断。
_Static_assert(PET_LAYOUT_STAGE_TOP_MIN + STAGE_H_REF - 1 <= PLAT_FLOOR_Y,
               "参照宠物自己得先满足顶边约束");
_Static_assert((PET_LAYOUT_SCR_W - STAGE_W_REF) / 2 + STAGE_W_REF <= PET_LAYOUT_SCR_W,
               "参照舞台居中后不能超出屏幕右边");

bool pet_layout_stage_supported(int stage_w, int stage_h)
{
    if (stage_w < PET_LAYOUT_STAGE_W_MIN || stage_w > PET_LAYOUT_STAGE_W_MAX) {
        return false;
    }
    if (stage_h < PET_LAYOUT_STAGE_H_MIN) return false;
    // 脚底固定在 PLAT_FLOOR_Y, 所以舞台越高顶边越靠上; 顶到状态行就不能要了。
    return PLAT_FLOOR_Y + 1 - stage_h >= PET_LAYOUT_STAGE_TOP_MIN;
}

bool pet_layout_for_stage(int stage_w, int stage_h, pet_layout_t *out)
{
    if (out == NULL || !pet_layout_stage_supported(stage_w, stage_h)) return false;

    pet_layout_t layout = {
        // 水平居中; 竖直方向按脚底对齐台面反推。
        .stage_x = (PET_LAYOUT_SCR_W - stage_w) / 2,
        .stage_y = PLAT_FLOOR_Y + 1 - stage_h,
        .stage_w = stage_w,
        .stage_h = stage_h,

        .plat_x = PLAT_X,
        .plat_w = PLAT_W,
        .plat_radius = PLAT_RADIUS,
        .plat_floor_y = PLAT_FLOOR_Y,
        .plat_floor_h = PLAT_FLOOR_H,
        .plat_floor_radius = PLAT_FLOOR_RADIUS,
        .plat_rim_h = PLAT_RIM_H,
        .plat_rim_inset = PLAT_RIM_INSET,
        .plat_body_y = PLAT_BODY_Y,
        .plat_body_h = PLAT_BODY_H,

        .plat_text_x = PLAT_X + (PLAT_W - PLAT_TEXT_W) / 2,
        .plat_text_y = PLAT_BODY_Y + PLAT_TEXT_PAD,
        .plat_text_w = PLAT_TEXT_W,

        .shadow_y = SHADOW_Y,
        .shadow_w = SHADOW_W,
        .shadow_h = SHADOW_H,

        .sleep_x = SLEEP_X,
        .sleep_y = SLEEP_Y,
    };

    // 运行时再兜一次: 上面的常量断言挡不住"传入的 stage 尺寸把舞台顶到状态行上"。
    if (layout.stage_y < PET_LAYOUT_STAGE_TOP_MIN) return false;

    *out = layout;
    return true;
}

bool pet_layout_frame_rect(int frame_x, int frame_y, int frame_w, int frame_h,
                           int crop_x0, int crop_y0,
                           const pet_layout_t *layout, pet_layout_rect_t *out)
{
    if (layout == NULL || out == NULL) return false;
    if (frame_w <= 0 || frame_h <= 0) return false;

    // 帧的裁剪框必须含在舞台裁剪框里 —— 这也是"裁剪原点减对了"的判据: 传进来的
    // crop_x0/crop_y0 要是屏幕上的舞台落点(比单元格原点大), 帧的裁剪框就会"跑到
    // 舞台外", 这里立刻拒绝。
    if (frame_x < crop_x0 || frame_y < crop_y0 ||
        frame_x + frame_w > crop_x0 + layout->stage_w ||
        frame_y + frame_h > crop_y0 + layout->stage_h) {
        return false;
    }

    // 舞台尺寸取自当前宠物的裁剪区并集, 而这里的裁剪原点取自同一个包头, 所以
    // 减完一定落在 [0, stage_w/h - w/h] 里。
    out->x = frame_x - crop_x0;
    out->y = frame_y - crop_y0;
    out->w = frame_w;
    out->h = frame_h;
    return true;
}
