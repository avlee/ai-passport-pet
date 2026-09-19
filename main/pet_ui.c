// main/pet_ui.c
#include "pet_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"  // bsp_lvgl_lock/unlock
#include "esp_log.h"
#include "lvgl.h"
#include "pet_atlas.h"
#include "pet_config.h"
#include "pet_fonts.h"
#include "pet_protocol.h"
#include "pet_strings.h"

static const char *TAG = "pet_ui";

// ---------------------------------------------------------------------------
// 布局。屏幕 240x320, 四角被 BSP 强制成 30 px 圆角(见 bsp_display.h), 所以
// 靠边的元素都要躲开角落的弧线, 下面这些数值是照着力学算好的:
//
//   y   6 ..  28   状态圆点 + 状态文字(左) / 电量(右)
//   y  38 .. 235   宠物舞台 129x198 —— 与图集裁剪区的并集等大, 不画底, 直接立在背景上
//   y 235 .. 314   站台: 台面 + 台身, 台面上沿压在宠物脚底那一行
//
// 舞台尺寸直接取自图集(PET_ATLAS_STAGE_*), 不写死: 换一只宠物时自动适配。
//
// 站台为什么贴到 235: 57 帧素材的裁剪区并集下沿都在图集 y=203, 换算到屏幕就是
// 脚底那一行(235)。把台面上沿放在这里, 宠物读起来就是"站在站台上", 而不是
// "浮在方块前"。素材换一份时这个值要跟着 PET_ATLAS_STAGE_H 重新对。
// ---------------------------------------------------------------------------
#define SCR_W 240
#define SCR_H 320

#define COL_BG         0x0B0F14
#define COL_CARD       0x16202A
#define COL_CARD_EDGE  0x22323F
#define COL_PLAT_FLOOR 0x223444  // 台面: 受光的那一段
#define COL_PLAT_RIM   0x3E6383  // 台面顶沿高光
#define COL_PLAT_BODY  0x131B24  // 台身: 承载文本的深色面
#define COL_INK        0xE8F1F5
#define COL_MUTED      0x7C93A3
#define COL_ACCENT     0x4CC2FF
#define COL_GOOD       0x4ADE80
#define COL_WARN       0xFBBF24
#define COL_BAD        0xF87171

#define ROW_DOT_X   18
#define ROW_DOT_Y   16
#define ROW_DOT_D   8

#define STATUS_X    32
#define STATUS_Y    5

#define BAT_TEXT_X  130
#define BAT_TEXT_Y  7
#define BAT_TEXT_W  74
#define BAT_BODY_X  208
#define BAT_BODY_Y  15
#define BAT_BODY_W  22
#define BAT_BODY_H  11
#define BAT_CAP_X   230
#define BAT_CAP_Y   18
#define BAT_CAP_W   3
#define BAT_CAP_H   5
#define BAT_FILL_X  210
#define BAT_FILL_Y  17
#define BAT_FILL_H  7
#define BAT_FILL_MAX_W 18

// 舞台: 129x198 的图集裁剪区并集, 水平居中(240 - 129 = 111 → 左边距 55)。
// 底行 235 就是宠物脚底, 站台的台面前沿要对在这里。
#define STAGE_X     55
#define STAGE_Y     38

// 站台。数值一律写字面量(不写成宏算式): tools/preview_pet_screen.py 用正则
// 直接读这些 #define 来出预览图, 算式会让它看不懂。等式由下面的
// _Static_assert 兜住 —— 改错了编译期就炸, 不会悄悄错位。
//
// 站台左右与屏幕中心对称: (240 - 212) / 2 = 14。
#define PLAT_X        14
#define PLAT_W       212
#define PLAT_RADIUS   14

// 台面上沿 = 宠物脚底那一行, 见文件头的布局说明。
#define PLAT_FLOOR_Y 235
#define PLAT_FLOOR_H  14
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

#define PLAT_TEXT_PAD  2
#define PLAT_TEXT_W  184

// 站台的几何约束。全部是整数常量表达式, 在编译期求值。
// 台面上沿必须正好落在舞台底行(= 素材里宠物的脚底), 否则宠物要么浮空、
// 要么脚被台面切掉。换宠物素材(不同 PET_ATLAS_STAGE_H)时这里会先报错。
_Static_assert(PLAT_FLOOR_Y == STAGE_Y + PET_ATLAS_STAGE_H - 1,
               "站台台面必须对齐宠物脚底(舞台底行)");
_Static_assert(PLAT_BODY_Y == PLAT_FLOOR_Y + PLAT_FLOOR_H,
               "台身必须紧接台面下沿");
_Static_assert(PLAT_X + PLAT_W == SCR_W - PLAT_X,
               "站台必须水平居中");
_Static_assert(PLAT_BODY_Y + PLAT_BODY_H <= SCR_H - 6,
               "站台底边要给屏幕底部的圆角和边框留出余量");
_Static_assert(SHADOW_Y >= PLAT_FLOOR_Y && SHADOW_Y + SHADOW_H <= PLAT_BODY_Y,
               "接触阴影必须整个落在台面上");
_Static_assert(PLAT_TEXT_W == PLAT_W - 28,
               "文本宽度 = 站台宽度减去左右各 14 px 内边距");

#define SLEEP_X     98
#define SLEEP_Y     6

#define INFO_X      10
#define INFO_Y      40
#define INFO_W      220
#define INFO_H      250

// 信息面板的行: 顺序与 INFO_ROW_* 一致。
enum {
    INFO_ROW_FIRMWARE = 0,
    INFO_ROW_PACKAGE,
    INFO_ROW_LINK,
    INFO_ROW_WIFI,
    INFO_ROW_BRIDGE,
    INFO_ROW_BATTERY,
    INFO_ROW_COUNT,
};

static const char *const INFO_ROW_LABELS[INFO_ROW_COUNT] = {
    PET_STR_INFO_FIRMWARE, PET_STR_INFO_PACKAGE, PET_STR_INFO_LINK,
    PET_STR_INFO_WIFI,     PET_STR_INFO_BRIDGE,  PET_STR_INFO_BATTERY,
};

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static lv_obj_t *s_screen;
static lv_obj_t *s_dot;
static lv_obj_t *s_status;
static lv_obj_t *s_bat_text;
static lv_obj_t *s_bat_fill;
static lv_obj_t *s_plat_body;
static lv_obj_t *s_plat_text;
static lv_obj_t *s_stage;
static lv_obj_t *s_sprite;
static lv_obj_t *s_sleep;

static lv_obj_t *s_info_scrim;
static lv_obj_t *s_info_panel;
static lv_obj_t *s_info_value[INFO_ROW_COUNT];

static lv_timer_t *s_anim_timer;

static pet_state_t       s_state;
static pet_settings_t    s_settings;
static bool              s_settings_ok;
static int               s_battery;

static pet_anim_t s_playing = PET_ANIM_COUNT;  // 当前装载的动作; COUNT = 尚未装载
static uint16_t   s_frame;
static bool       s_override;                  // 正在播用户触发的一次性动作
static pet_anim_t s_override_anim;
static char       s_text[PET_PROTOCOL_TEXT_MAX];
static bool       s_info_visible;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

// ---------------------------------------------------------------------------
// 文案映射
// ---------------------------------------------------------------------------
static const char *status_text(pet_link_state_t link, pet_codex_state_t codex)
{
    switch (link) {
    case PET_LINK_OFFLINE:    return PET_STR_STATUS_OFFLINE;
    case PET_LINK_CONNECTING: return PET_STR_STATUS_CONNECTING;
    case PET_LINK_ONLINE:
        switch (codex) {
        case PET_CODEX_WORKING: return PET_STR_STATUS_WORKING;
        case PET_CODEX_WAITING: return PET_STR_STATUS_WAITING;
        case PET_CODEX_READY:   return PET_STR_STATUS_READY;
        case PET_CODEX_FAILED:  return PET_STR_STATUS_FAILED;
        case PET_CODEX_IDLE:
        default:                return PET_STR_STATUS_IDLE;
        }
    default:                  return PET_STR_STATUS_OFFLINE;
    }
}

static uint32_t status_color(pet_link_state_t link, pet_codex_state_t codex)
{
    switch (link) {
    case PET_LINK_OFFLINE:    return COL_MUTED;
    case PET_LINK_CONNECTING: return COL_WARN;
    case PET_LINK_ONLINE:
        switch (codex) {
        case PET_CODEX_WORKING: return COL_ACCENT;
        case PET_CODEX_WAITING: return COL_WARN;
        case PET_CODEX_READY:   return COL_GOOD;
        case PET_CODEX_FAILED:  return COL_BAD;
        case PET_CODEX_IDLE:
        default:                return COL_GOOD;
        }
    default:                  return COL_MUTED;
    }
}

static const char *placeholder_text(pet_link_state_t link,
                                    pet_codex_state_t codex)
{
    switch (link) {
    case PET_LINK_OFFLINE:    return PET_STR_PH_OFFLINE;
    case PET_LINK_CONNECTING: return PET_STR_PH_CONNECTING;
    case PET_LINK_ONLINE:
        switch (codex) {
        case PET_CODEX_WORKING: return PET_STR_PH_WORKING;
        case PET_CODEX_WAITING: return PET_STR_PH_WAITING;
        case PET_CODEX_READY:   return PET_STR_PH_READY;
        case PET_CODEX_FAILED:  return PET_STR_PH_FAILED;
        case PET_CODEX_IDLE:
        default:                return PET_STR_PH_IDLE;
        }
    default:                  return PET_STR_PH_OFFLINE;
    }
}

// 头部状态行 + 气泡占位都跟着链路/Codex 状态走, 所以集中在一处刷新。
// 调用方必须已持有 LVGL 锁。
static void refresh_status_locked(void)
{
    if (s_screen == NULL) return;

    const pet_link_state_t link = s_state.link;
    const pet_codex_state_t codex = s_state.codex;

    lv_label_set_text(s_status, status_text(link, codex));
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(status_color(link, codex)), 0);

    // 用户没有下发文本时, 用占位文案说明当前在等什么。
    if (s_text[0] == '\0') {
        lv_label_set_text(s_plat_text, placeholder_text(link, codex));
    }
    lv_obj_set_style_text_color(
        s_plat_text,
        lv_color_hex(s_text[0] != '\0' ? COL_INK : COL_MUTED), 0);

    const bool asleep = (link == PET_LINK_OFFLINE);
    lv_obj_set_style_image_opa(s_sprite, asleep ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (asleep) {
        lv_obj_remove_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_battery_locked(void)
{
    if (s_bat_text == NULL) return;

    if (s_battery < 0) {
        lv_label_set_text(s_bat_text, "--");
        lv_obj_set_style_text_color(s_bat_text, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_TRANSP, 0);
        return;
    }

    lv_label_set_text_fmt(s_bat_text, "%d%%", s_battery);

    const uint32_t color = s_battery >= 50 ? COL_GOOD
                         : s_battery >= 20 ? COL_WARN
                                           : COL_BAD;
    lv_obj_set_style_text_color(s_bat_text, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_COVER, 0);

    int width = (s_battery * BAT_FILL_MAX_W) / 100;
    if (width < 1) width = 1;
    lv_obj_set_size(s_bat_fill, width, BAT_FILL_H);
}

// 信息面板里的动态行: 链路 / 电量。
static void refresh_info_locked(void)
{
    if (s_info_panel == NULL) return;

    lv_label_set_text(s_info_value[INFO_ROW_LINK],
                      status_text(s_state.link, s_state.codex));

    lv_label_set_text(s_info_value[INFO_ROW_FIRMWARE], PET_FIRMWARE_VERSION);
    lv_label_set_text(s_info_value[INFO_ROW_PACKAGE], PET_PACKAGE_ID);
    lv_label_set_text(s_info_value[INFO_ROW_WIFI],
                      s_settings_ok && s_settings.ssid[0] != '\0'
                          ? s_settings.ssid
                          : PET_STR_INFO_UNKNOWN);

    if (s_settings_ok) {
        lv_label_set_text_fmt(s_info_value[INFO_ROW_BRIDGE], "%s:%u",
                              s_settings.host, (unsigned)s_settings.port);
    } else {
        lv_label_set_text(s_info_value[INFO_ROW_BRIDGE], PET_STR_INFO_UNKNOWN);
    }

    if (s_battery < 0) {
        lv_label_set_text(s_info_value[INFO_ROW_BATTERY], PET_STR_INFO_UNKNOWN);
    } else {
        lv_label_set_text_fmt(s_info_value[INFO_ROW_BATTERY], "%d%%", s_battery);
    }
}

// 面板显隐。做成 locked 版本, 因为状态更新路径自己已经持锁, 不能再进公共入口
// (LVGL 的锁不可重入)。
static void set_info_visible_locked(bool visible)
{
    s_info_visible = visible;
    if (s_info_scrim == NULL) return;

    if (visible) {
        refresh_info_locked();
        lv_obj_remove_flag(s_info_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_info_scrim);
    } else {
        lv_obj_add_flag(s_info_scrim, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// 动画驱动
// ---------------------------------------------------------------------------
// 把「当前动作 + 当前帧」真正画到屏幕上, 并把下一帧的间隔交给定时器。
// 这是整个界面唯一的绘制入口, 所以帧同步问题只需要在这里想清楚。
static void render_frame(void)
{
    const uint16_t count = pet_atlas_frame_count(s_playing);
    if (count == 0) return;
    if (s_frame >= count) s_frame = 0;

    const pet_atlas_frame_t *frame = pet_atlas_frame(s_playing, s_frame);
    const lv_image_dsc_t *image = pet_atlas_image(s_playing, s_frame);
    if (frame == NULL || image == NULL) return;

    lv_image_set_src(s_sprite, image);

    // 关键: 每帧在图集单元格里的裁剪原点不同, 扣掉舞台原点才是它在屏上的位置。
    // 动作里的位移(跑动、跳跃的横向移动)就是这么来的 —— 与 ChatGPT 一致,
    // 不是我们额外加的补间。
    lv_obj_set_pos(s_sprite, frame->x - PET_ATLAS_STAGE_X,
                   frame->y - PET_ATLAS_STAGE_Y);

    uint32_t period = frame->duration;
    const pet_pose_t pose = pet_state_pose(&s_state);
    if (pose.speed_pct != 100) {
        period = period * pose.speed_pct / 100;
    }
    if (period < 30) period = 30;  // 防止素材写错导致定时器空转
    lv_timer_set_period(s_anim_timer, period);
}

// 设定要播的动作, 并立刻从第 0 帧开始。
static void load_anim(pet_anim_t anim)
{
    s_playing = anim;
    s_frame = 0;
    render_frame();
}

static void anim_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_screen == NULL) return;

    const pet_pose_t pose = pet_state_pose(&s_state);
    const pet_anim_t want = s_override ? s_override_anim : pose.anim;
    const uint16_t count = pet_atlas_frame_count(want);
    if (count == 0) {
        lv_timer_set_period(s_anim_timer, 200);
        return;
    }

    // 动作变了: 从第 0 帧重新起播。
    if (want != s_playing) {
        load_anim(want);
        return;
    }

    // 本动作还有下一帧。
    if (s_frame + 1 < count) {
        s_frame++;
        render_frame();
        return;
    }

    // 刚好播完最后一帧, 决定接下来播什么。
    if (s_override) {
        // 用户触发的一次性动作结束, 交还给同步状态。
        s_override = false;
    } else if (pose.once) {
        // 状态机的过场动画(挥手/跳跃庆祝)结束, 推进到下一个循环动作。
        pet_state_oneshot_complete(&s_state);
    } else {
        s_frame = 0;  // 循环动作: 回第 0 帧
        render_frame();
        return;
    }

    // 让下一拍立刻重新装载动作, 中间不留空档。
    s_playing = PET_ANIM_COUNT;
    lv_timer_set_period(s_anim_timer, 1);
}

// ---------------------------------------------------------------------------
// 构建
// ---------------------------------------------------------------------------
static void build_top_row(void)
{
    // 状态圆点: 一眼看出链路/任务状态, 比读文字快。
    s_dot = make_box(s_screen, ROW_DOT_X, ROW_DOT_Y, ROW_DOT_D, ROW_DOT_D,
                     COL_MUTED, ROW_DOT_D / 2);
    s_status = make_label(s_screen, "", pet_font_title(), COL_INK);
    lv_obj_set_pos(s_status, STATUS_X, STATUS_Y);

    s_bat_text = make_label(s_screen, "--", pet_font_body(), COL_MUTED);
    lv_obj_set_size(s_bat_text, BAT_TEXT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_bat_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_bat_text, BAT_TEXT_X, BAT_TEXT_Y);

    lv_obj_t *body = make_box(s_screen, BAT_BODY_X, BAT_BODY_Y, BAT_BODY_W,
                              BAT_BODY_H, COL_MUTED, 3);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(COL_MUTED), 0);

    make_box(s_screen, BAT_CAP_X, BAT_CAP_Y, BAT_CAP_W, BAT_CAP_H, COL_MUTED, 1);

    s_bat_fill = make_box(s_screen, BAT_FILL_X, BAT_FILL_Y, 1, BAT_FILL_H,
                          COL_GOOD, 1);
    lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_TRANSP, 0);
}

static void build_stage(void)
{
    // 舞台不画任何东西, 只负责把宠物夹在自己的范围里, 并给帧位置一个原点。
    // 宠物身下不留底色: 多一层浅色卡片会把轮廓框住, 动作幅度反而看不清;
    // "地面"由站台和脚底阴影交代, 见 build_platform()。
    s_stage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_stage);
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_stage, STAGE_X, STAGE_Y);
    lv_obj_set_size(s_stage, PET_ATLAS_STAGE_W, PET_ATLAS_STAGE_H);
    lv_obj_set_style_bg_opa(s_stage, LV_OPA_TRANSP, 0);

    s_sprite = lv_image_create(s_stage);
    lv_obj_remove_style_all(s_sprite);
    lv_obj_set_pos(s_sprite, 0, 0);

    // 睡眠标记浮在宠物右上方那块空白上(素材头部只占左半边)。
    s_sleep = make_label(s_stage, PET_STR_SLEEP_MARK, pet_font_body(),
                         COL_ACCENT);
    lv_obj_set_pos(s_sleep, SLEEP_X, SLEEP_Y);
    lv_obj_set_style_text_opa(s_sleep, LV_OPA_70, 0);
}

// 站台: 台身(深) + 台面(浅) + 台面顶沿高光 + 脚底接触阴影, 四层叠出"一块板"。
// 全部建在舞台之前 —— LVGL 按创建顺序决定叠放, 宠物必须压在台面之上,
// 否则脚会被台面盖掉, 变成"站在台子后面"。
static void build_platform(void)
{
    s_plat_body = make_box(s_screen, PLAT_X, PLAT_BODY_Y, PLAT_W, PLAT_BODY_H,
                           COL_PLAT_BODY, PLAT_RADIUS);
    lv_obj_set_style_border_width(s_plat_body, 1, 0);
    lv_obj_set_style_border_color(s_plat_body, lv_color_hex(COL_CARD_EDGE), 0);

    // 台面: 竖直渐变让上沿亮、下沿并入台身, 接缝不生硬。
    lv_obj_t *floor = make_box(s_screen, PLAT_X, PLAT_FLOOR_Y, PLAT_W,
                               PLAT_FLOOR_H, COL_PLAT_FLOOR,
                               PLAT_FLOOR_RADIUS);
    lv_obj_set_style_bg_grad_dir(floor, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(floor, lv_color_hex(COL_PLAT_BODY), 0);

    // 顶沿高光: 只留中间一段, 两端收进去, 避免和台面圆角打架。
    make_box(s_screen, PLAT_X + PLAT_RIM_INSET, PLAT_FLOOR_Y,
             PLAT_W - PLAT_RIM_INSET * 2, PLAT_RIM_H,
             COL_PLAT_RIM, PLAT_RIM_H / 2);

    // 接触阴影: 半透明黑压在台面上, 位置取自脚底正下方。跳跃动作抬起时
    // 阴影不动 —— 它是"地面", 不是宠物的挂件。
    lv_obj_t *shadow = make_box(s_screen, SCR_W / 2 - SHADOW_W / 2, SHADOW_Y,
                                SHADOW_W, SHADOW_H, 0x000000, SHADOW_H / 2);
    lv_obj_set_style_bg_opa(shadow, LV_OPA_40, 0);

    // 文字落在台身上: 台身就是原来的气泡, 只是换了块"水泥"。
    const lv_font_t *font = pet_font_body();
    const int text_h = font->line_height * 2;
    s_plat_text = make_label(s_screen, "", font, COL_MUTED);
    lv_obj_set_size(s_plat_text, PLAT_TEXT_W, text_h);
    lv_label_set_long_mode(s_plat_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_plat_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_plat_text, PLAT_X + (PLAT_W - PLAT_TEXT_W) / 2,
                   PLAT_BODY_Y + PLAT_TEXT_PAD);
}

static void build_info(void)
{
    s_info_scrim = make_box(s_screen, 0, 0, SCR_W, SCR_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(s_info_scrim, LV_OPA_70, 0);

    s_info_panel = make_box(s_info_scrim, INFO_X, INFO_Y, INFO_W, INFO_H,
                            COL_CARD, 18);
    lv_obj_set_style_border_width(s_info_panel, 1, 0);
    lv_obj_set_style_border_color(s_info_panel, lv_color_hex(COL_CARD_EDGE), 0);

    lv_obj_t *title = make_label(s_info_panel, PET_STR_INFO_TITLE,
                                 pet_font_title(), COL_INK);
    lv_obj_set_pos(title, 14, 12);

    for (int i = 0; i < INFO_ROW_COUNT; i++) {
        lv_obj_t *key = make_label(s_info_panel, INFO_ROW_LABELS[i],
                                   pet_font_body(), COL_MUTED);
        lv_obj_set_pos(key, 14, 48 + i * 24);

        s_info_value[i] = make_label(s_info_panel, PET_STR_INFO_UNKNOWN,
                                     pet_font_body(), COL_INK);
        lv_obj_set_size(s_info_value[i], 120, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(s_info_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_info_value[i], INFO_W - 14 - 120, 48 + i * 24);
    }

    lv_obj_t *hint_close = make_label(s_info_panel, PET_STR_INFO_HINT_CLOSE,
                                      pet_font_body(), COL_MUTED);
    lv_obj_align(hint_close, LV_ALIGN_BOTTOM_MID, 0, -26);

    lv_obj_t *hint_demo = make_label(s_info_panel, PET_STR_INFO_HINT_DEMO,
                                     pet_font_body(), COL_MUTED);
    lv_obj_align(hint_demo, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_add_flag(s_info_scrim, LV_OBJ_FLAG_HIDDEN);
}

void pet_ui_build(void)
{
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁, 界面未创建");
        return;
    }
    if (s_screen != NULL) {
        bsp_lvgl_unlock();
        return;
    }

    // build/destroy 只负责对象, 状态(链路、Codex、文案、电量)留在模块里,
    // 这样从演示菜单回来重建界面时不会丢失已经同步到的状态。
    s_playing = PET_ANIM_COUNT;
    s_frame = 0;
    s_override = false;

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_screen_load(s_screen);

    build_top_row();
    build_platform();   // 先于舞台: 宠物要压在台面之上, 否则脚会被盖住
    build_stage();
    build_info();
    refresh_status_locked();
    refresh_battery_locked();
    set_info_visible_locked(s_info_visible);  // 重建时恢复面板显隐

    // 开机第一拍: 让定时器把第 0 帧画出来。
    s_anim_timer = lv_timer_create(anim_tick, 1, NULL);

    ESP_LOGI(TAG, "界面就绪: 舞台 %dx%d @ (%d,%d) 脚底 y=%d, 站台 %d..%d, 行高 %d",
             PET_ATLAS_STAGE_W, PET_ATLAS_STAGE_H, STAGE_X, STAGE_Y, PLAT_FLOOR_Y,
             PLAT_FLOOR_Y, PLAT_BODY_Y + PLAT_BODY_H,
             pet_font_body()->line_height);
    bsp_lvgl_unlock();
}

void pet_ui_init(void)
{
    pet_state_init(&s_state);
    s_text[0] = '\0';
    s_battery = -1;
    s_info_visible = false;
    s_settings_ok = false;
}

void pet_ui_destroy(void)
{
    if (!bsp_lvgl_lock(1000)) return;

    if (s_anim_timer != NULL) {
        lv_timer_delete(s_anim_timer);
        s_anim_timer = NULL;
    }
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
    }

    s_screen = NULL;
    s_sprite = NULL;
    s_stage = NULL;
    s_plat_body = NULL;
    s_plat_text = NULL;
    s_dot = NULL;
    s_status = NULL;
    s_bat_text = NULL;
    s_bat_fill = NULL;
    s_sleep = NULL;
    s_info_scrim = NULL;
    s_info_panel = NULL;
    for (int i = 0; i < INFO_ROW_COUNT; i++) s_info_value[i] = NULL;
    s_playing = PET_ANIM_COUNT;
    s_frame = 0;
    s_override = false;

    bsp_lvgl_unlock();
}

bool pet_ui_ready(void)
{
    return s_screen != NULL;
}

// ---------------------------------------------------------------------------
// 对外更新接口
// ---------------------------------------------------------------------------
void pet_ui_set_link(pet_link_state_t link)
{
    if (!bsp_lvgl_lock(1000)) return;
    if (pet_state_set_link(&s_state, link) && s_screen != NULL) {
        ESP_LOGI(TAG, "链路 -> %d, 动画 -> %s", (int)link,
                 pet_anim_name(s_state.anim));
        refresh_status_locked();
        refresh_info_locked();
    }
    bsp_lvgl_unlock();
}

void pet_ui_set_codex(pet_codex_state_t codex)
{
    if (!bsp_lvgl_lock(1000)) return;
    if (pet_state_set_codex(&s_state, codex) && s_screen != NULL) {
        ESP_LOGI(TAG, "Codex -> %s, 动画 -> %s", pet_codex_state_name(codex),
                 pet_anim_name(s_state.anim));
        refresh_status_locked();
        pet_ui_set_info_visible(s_info_visible);
    }
    bsp_lvgl_unlock();
}

void pet_ui_set_text(const char *utf8)
{
    if (!bsp_lvgl_lock(1000)) return;

    if (utf8 == NULL) {
        s_text[0] = '\0';
    } else {
        // 协议层已经限长, 这里只保证一定以 NUL 结尾。
        snprintf(s_text, sizeof(s_text), "%s", utf8);
    }

    if (s_screen != NULL) {
        if (s_text[0] != '\0') {
            lv_label_set_text(s_plat_text, s_text);
        }
        refresh_status_locked();
    }
    bsp_lvgl_unlock();
}

void pet_ui_set_battery(int soc_percent)
{
    if (!bsp_lvgl_lock(1000)) return;
    s_battery = soc_percent;
    refresh_battery_locked();
    refresh_info_locked();
    bsp_lvgl_unlock();
}

void pet_ui_set_settings(const pet_settings_t *settings)
{
    if (!bsp_lvgl_lock(1000)) return;
    if (settings != NULL) {
        s_settings = *settings;
        s_settings_ok = true;
    } else {
        s_settings_ok = false;
    }
    refresh_info_locked();
    bsp_lvgl_unlock();
}

void pet_ui_emote(pet_anim_t anim)
{
    if (anim < 0 || anim >= PET_ANIM_COUNT) return;
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen != NULL) {
        s_override = true;
        s_override_anim = anim;
        load_anim(anim);
        lv_timer_reset(s_anim_timer);  // 从这一帧重新计时, 避免等上一帧的余量
    }
    bsp_lvgl_unlock();
}

void pet_ui_set_info_visible(bool visible)
{
    if (!bsp_lvgl_lock(1000)) return;
    set_info_visible_locked(visible);
    bsp_lvgl_unlock();
}

bool pet_ui_info_visible(void)
{
    return s_info_visible;
}

pet_anim_t pet_ui_current_anim(void)
{
    return s_playing;
}
