// tests/test_pet_layout.c
// 宠物版式的单元测试(主机)。
//
// 舞台位置算错是**静默**的: 宠物整体偏几像素, 编译不报错、运行不打日志, 只有盯着
// 屏幕看才发现。换宠物之前 STAGE_X 是写死的 55(照 129 px 宽的舞台算的), 换成
// 156 px 宽的那只就会明显偏右 —— 这里把那类错误全部钉死。
#include <stdbool.h>
#include <stdio.h>

#include "pet_layout.h"

static int g_failures;

static void check(bool condition, const char *what)
{
    if (condition) return;
    printf("FAIL: %s\n", what);
    g_failures++;
}

// 舞台水平居中, 而且不许出屏。
static void check_centered(int stage_w)
{
    pet_layout_t layout;
    if (!pet_layout_for_stage(stage_w, 198, &layout)) {
        printf("FAIL: stage_w=%d should be supported\n", stage_w);
        g_failures++;
        return;
    }

    const int expected = (PET_LAYOUT_SCR_W - stage_w) / 2;
    if (layout.stage_x != expected) {
        printf("FAIL: stage_w=%d -> stage_x=%d, expected %d\n", stage_w,
               layout.stage_x, expected);
        g_failures++;
    }
    // 居中之后左右两边的余量最多差 1 px(奇数宽度时不可能完全对称)。
    const int left = layout.stage_x;
    const int right = PET_LAYOUT_SCR_W - (layout.stage_x + layout.stage_w);
    if (right < 0) {
        printf("FAIL: stage_w=%d overflows the screen\n", stage_w);
        g_failures++;
    }
    if (left - right > 1 || right - left > 1) {
        printf("FAIL: stage_w=%d is off-centre (%d vs %d)\n", stage_w, left, right);
        g_failures++;
    }
}

static void test_reference_pet_is_unchanged(void)
{
    // 参照宠物(sophie-portrait)必须画出和重构前一模一样的版式: 这是唯一一处
    // 可以拿来回归的"已知正确"。
    pet_layout_t layout;
    check(pet_layout_for_stage(129, 198, &layout), "reference stage is supported");
    check(layout.stage_x == 55, "reference stage keeps its old left edge");
    check(layout.stage_y == 38, "reference stage keeps its old top edge");
    check(layout.plat_floor_y == 235, "platform floor stays put");
    check(layout.plat_body_y == 249, "platform body stays put");
    check(layout.plat_text_y == 251, "text block stays put");
}

static void test_every_supported_width_is_centred(void)
{
    for (int w = PET_LAYOUT_STAGE_W_MIN; w <= PET_LAYOUT_STAGE_W_MAX; w++) {
        check_centered(w);
    }
}

static void test_feet_always_land_on_the_platform(void)
{
    // 脚底那一行 = stage_y + stage_h - 1, 必须永远等于台面上沿。
    for (int h = PET_LAYOUT_STAGE_H_MIN; h <= 206; h++) {
        pet_layout_t layout;
        if (!pet_layout_for_stage(129, h, &layout)) continue;
        if (layout.stage_y + layout.stage_h - 1 != layout.plat_floor_y) {
            printf("FAIL: stage_h=%d does not land on the platform\n", h);
            g_failures++;
        }
        if (layout.stage_y < PET_LAYOUT_STAGE_TOP_MIN) {
            printf("FAIL: stage_h=%d overlaps the status row\n", h);
            g_failures++;
        }
    }
}

static void test_known_pets_fit(void)
{
    // 手上的两只: sophie-portrait 129x198, li-muwan 156x198。
    check(pet_layout_stage_supported(129, 198), "sophie-portrait fits");
    check(pet_layout_stage_supported(156, 198), "li-muwan fits");

    pet_layout_t layout;
    check(pet_layout_for_stage(156, 198, &layout), "li-muwan lays out");
    check(layout.stage_x == 42, "li-muwan is centred at 42");
    check(layout.plat_floor_y == 235, "li-muwan keeps the platform");
}

static void test_rejects_impossible_stages(void)
{
    pet_layout_t layout;
    check(!pet_layout_for_stage(0, 198, &layout), "zero width is rejected");
    check(!pet_layout_for_stage(PET_LAYOUT_STAGE_W_MAX + 1, 198, &layout),
          "over-wide stage is rejected");
    check(!pet_layout_for_stage(129, 32, &layout), "tiny stage is rejected");
    // 顶边压到状态行: 舞台下沿固定在 235, 所以太高的舞台一定会撞上去。
    check(!pet_layout_for_stage(129, PET_LAYOUT_SCR_H, &layout),
          "over-tall stage is rejected");
    check(!pet_layout_stage_supported(129, 207), "207 px is one pixel too tall");
    check(pet_layout_stage_supported(129, 206), "206 px still fits");
}

static void test_platform_stays_inside_the_screen(void)
{
    pet_layout_t layout;
    check(pet_layout_for_stage(129, 198, &layout), "layout computed");
    check(layout.plat_body_y + layout.plat_body_h <= PET_LAYOUT_SCR_H,
          "platform stays on screen");
    check(layout.plat_x >= 0 && layout.plat_x + layout.plat_w <= PET_LAYOUT_SCR_W,
          "platform stays horizontally inside");
    check(layout.plat_text_x >= layout.plat_x &&
              layout.plat_text_x + layout.plat_text_w <= layout.plat_x + layout.plat_w,
          "text block stays inside the platform");
    check(layout.shadow_y >= layout.plat_floor_y &&
              layout.shadow_y + layout.shadow_h <= layout.plat_body_y,
          "contact shadow stays on the platform floor");
}

static void test_frame_landing_uses_the_crop_origin(void)
{
    // 参照宠物(sophie-portrait)的真值: 裁剪区并集 129x198 @ (31,5) 在单元格里,
    // 舞台上屏后落在 (55,38)。这两个 (x,y) 都叫"舞台原点", 但在两个坐标系里。
    pet_layout_t layout;
    check(pet_layout_for_stage(129, 198, &layout), "reference layout computed");

    pet_layout_rect_t rect;

    // 并集的左上角那一帧 -> 舞台的左上角。
    check(pet_layout_frame_rect(31, 5, 129, 198, 31, 5, &layout, &rect),
          "the union's own frame lands at the origin");
    check(rect.x == 0 && rect.y == 0, "union frame -> (0,0)");

    // 帧动了, 落点跟着动, 而且整帧必须还在舞台里。
    check(pet_layout_frame_rect(60, 24, 40, 40, 31, 5, &layout, &rect),
          "an inner frame lands inside the stage");
    check(rect.x == 29 && rect.y == 19, "inner frame -> (29,19)");

    // 右下角贴边的那一帧不算越界。
    check(pet_layout_frame_rect(31 + 129 - 8, 5 + 198 - 8, 8, 8, 31, 5,
                                &layout, &rect),
          "a frame flush with the bottom-right corner is allowed");
    check(rect.x == 121 && rect.y == 190, "flush frame lands at (121,190)");

    // **这条是本文件存在的理由**: 拿屏幕落点(55,38)当裁剪原点来减, 必须被拒。
    // 这个错不会编译失败、不会崩, 只会让宠物左上偏 24/33 px 并被舞台裁掉一角 ——
    // 真机上第一次换宠物就是这么错的。
    check(!pet_layout_frame_rect(31, 5, 129, 198, layout.stage_x, layout.stage_y,
                                 &layout, &rect),
          "subtracting the on-screen stage position is rejected");
    check(!pet_layout_frame_rect(31, 5, 129, 198, 55, 38, &layout, &rect),
          "the literal (55,38) mistake is rejected");

    // 越出舞台裁剪框的帧同样要拒(包坏了才会出现)。
    check(!pet_layout_frame_rect(31, 5, 130, 198, 31, 5, &layout, &rect),
          "a frame wider than the stage is rejected");
    check(!pet_layout_frame_rect(31, 5, 129, 199, 31, 5, &layout, &rect),
          "a frame taller than the stage is rejected");

    // 换一只更宽的宠物: 舞台落点变了(42), 但裁剪原点跟它无关 —— 落点仍从 0 算起。
    check(pet_layout_for_stage(156, 198, &layout), "wider pet lays out");
    check(pet_layout_frame_rect(18, 7, 156, 198, 18, 7, &layout, &rect),
          "wider pet: union frame lands at the origin");
    check(rect.x == 0 && rect.y == 0,
          "wider pet: the on-screen position (42) must not leak into the offset");
}

int main(void)
{
    test_reference_pet_is_unchanged();
    test_every_supported_width_is_centred();
    test_feet_always_land_on_the_platform();
    test_known_pets_fit();
    test_rejects_impossible_stages();
    test_platform_stays_inside_the_screen();
    test_frame_landing_uses_the_crop_origin();

    if (g_failures != 0) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("pet layout tests: OK\n");
    return 0;
}
