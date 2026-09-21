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
#include "pet_layout.h"
#include "pet_protocol.h"
#include "pet_strings.h"

static const char *TAG = "pet_ui";

// ---------------------------------------------------------------------------
// 布局
//
// 屏幕几何(240x320、四角被 BSP 切 30 px 圆角)与站台/舞台的坐标全部在
// pet_layout.c 里 —— 那边是纯逻辑, 有主机测试, 也有 tools/preview_pet_screen.py
// 用的同一批字面量。这里只负责把算出来的坐标摆到 LVGL 对象上。
//
// 舞台尺寸现在是**运行时**的: 每只宠物图集裁剪区的并集不同(sophie-portrait
// 129x198、li-muwan 156x198), pet_ui_build() 会先问图集要尺寸再算版式。以前
// STAGE_X 是写死的 55(照 129 宽的舞台算的), 换成 156 宽的那只就会静默偏右。
//
// 下面顶栏(ROW_DOT_*/STATUS_*/BAT_*)与 INFO_*/PROV_*/TRANS_* 都与宠物尺寸无关,
// 是固定版式, 一律写成整数字面量: 预览工具用正则直接读它们出预览图, 写成宏算式
// 会被静默漏掉, 等式由 _Static_assert 兜住。
// ---------------------------------------------------------------------------
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

// 多行文本的行距(LVGL text_line_space, 加在行与行之间, 首行位置不变)。默认按
// 字体行高走, 在 240px 宽的小屏上换行文字显得松; 收紧一档。单行 label 不受影响。
// tools/preview_pet_screen.py 从本文件解析同一个值, 改这里预览跟着变。
#define TEXT_LINE_SPACE -1

// 顶栏: 左 = 状态圆点 + 状态文字, 右 = 电量(数字 + 电池图形)。
#define ROW_DOT_X   18
#define ROW_DOT_Y   16
#define ROW_DOT_D    8
#define STATUS_X    32
#define STATUS_Y     5

#define BAT_TEXT_X   130
#define BAT_TEXT_Y     7
#define BAT_TEXT_W    74
#define BAT_BODY_X   208
#define BAT_BODY_Y    15
#define BAT_BODY_W    22
#define BAT_BODY_H    11
#define BAT_CAP_X    230
#define BAT_CAP_Y     18
#define BAT_CAP_W      3
#define BAT_CAP_H      5
#define BAT_FILL_X   210
#define BAT_FILL_Y    17
#define BAT_FILL_H     7
#define BAT_FILL_MAX_W 18

#define INFO_X      10
#define INFO_Y      40
#define INFO_W      220
#define INFO_H      250

_Static_assert(STATUS_X + 88 <= BAT_TEXT_X,
               "状态文字要给右边的电量让出位置(最长那条 4 个汉字 = 80 px)");
_Static_assert(BAT_BODY_X + BAT_BODY_W + BAT_CAP_W <= PET_LAYOUT_SCR_W,
               "电池图形不能超出屏幕右边");
_Static_assert(BAT_CAP_X == BAT_BODY_X + BAT_BODY_W,
               "电池正极要贴在电池体右侧");
_Static_assert(BAT_FILL_X > BAT_BODY_X && BAT_FILL_X + BAT_FILL_MAX_W < BAT_CAP_X,
               "电量填充必须落在电池体内");

// Codex 用量能量槽: 站台台身(文字框)内左右两条竖槽 —— 左 = 5 小时窗(红),
// 右 = 周窗(蓝)。颜色 + 位置 + 小标签三重编码, 新观众也能对上号。
// 填充按**剩余**百分比从槽底往上生长。槽的坐标完全锚定运行时的站台几何
// (s_layout), 预览工具用镜像的同一批值, 所以这里只写字面量尺寸。
// 标签放槽底而不是槽顶: 台身顶部到中部是居中的状态文字, 标签放上面会跟它
// 抢空间; 底部内缩区与文字完全错开。
#define GAUGE_W            4   // 槽宽(轨道与填充同宽)
#define GAUGE_INSET        4   // 距台身左右边框的水平内缩
#define GAUGE_VPAD         8   // 距台身顶的竖直内缩, 避开 14px 圆角弧
#define GAUGE_MIN_H        3   // 有剩余时的最小可见高度, 否则 1% 看起来像空的
#define GAUGE_LABEL_H     16   // 槽底小标签("5h"/"7d")的行高, 从槽底高度里扣
#define GAUGE_LABEL_GAP    1   // 标签与槽底贴近, 把竖直空间尽量留给槽体
#define GAUGE_LABEL_W     26   // 标签行框宽(左标签左对齐/右标签右对齐于槽)
#define GAUGE_BOTTOM_CLEAR 4   // 标签底与台身底边的净空。压线但不出弧: 标签最底
                               // 行(y≈310)处 14px 圆角弧内收约 4.2px, 标签自身
                               // 内缩 4px, 视觉上贴着弧线走

// 蓝牙配网页。同样只写字面量: tools/preview_pet_screen.py 会读这些值出预览图,
// 编辑器里看不到屏幕, 预览就是唯一能"眼见为实"的途径。
#define PROV_TITLE_Y     26
#define PROV_NAME_Y      56
#define PROV_CARD_X      30
#define PROV_CARD_Y      88
#define PROV_CARD_W     180
#define PROV_CARD_H      86
#define PROV_PIN_LABEL_Y 10
#define PROV_PIN_Y       36
#define PROV_STATUS_X    20
#define PROV_STATUS_Y   192
#define PROV_STATUS_W   200
#define PROV_STATUS_H    62
#define PROV_HINT_Y     268

_Static_assert(PROV_CARD_Y + PROV_CARD_H < PROV_STATUS_Y,
               "配对码卡片不能压到状态文字");
_Static_assert(PROV_STATUS_Y + PROV_STATUS_H < PROV_HINT_Y,
               "状态文字不能压到底部提示");
_Static_assert(PROV_HINT_Y + 32 <= PET_LAYOUT_SCR_H,
               "底部提示要给屏幕圆角留出余量");

// 宠物传输页。换宠物时宠物界面必须先整个拆掉(图集要解除映射), 但黑屏几秒钟会
// 让人以为设备坏了 —— 这块屏幕顶上, 只显示"在收什么、收到哪了"。
// 名字是 ASCII 的宠物 id, 最坏 31 字节, 靠 DOTS 模式省略, 所以只给宽度不设断言。
#define TRANS_TITLE_Y   112
#define TRANS_NAME_Y    142
#define TRANS_BAR_X      40
#define TRANS_BAR_Y     178
#define TRANS_BAR_W     160
#define TRANS_BAR_H      10
#define TRANS_MSG_X      20
#define TRANS_MSG_Y     200
#define TRANS_MSG_W     200
#define TRANS_MSG_H      64

_Static_assert(TRANS_BAR_X + TRANS_BAR_W <= PET_LAYOUT_SCR_W,
               "进度条不能超出屏幕");
_Static_assert(TRANS_MSG_X + TRANS_MSG_W <= PET_LAYOUT_SCR_W,
               "说明文字不能超出屏幕");
_Static_assert(TRANS_BAR_Y < TRANS_MSG_Y,
               "说明文字要在进度条下面");
_Static_assert(TRANS_MSG_Y + TRANS_MSG_H + 32 <= PET_LAYOUT_SCR_H,
               "说明文字要给屏幕底部圆角留出余量");

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
// 当前版式。由 pet_ui_build() 按**当前宠物**的舞台尺寸算出来 —— 换宠物之后
// 舞台尺寸会变, 所以这些坐标不能是编译期常量。s_layout_ok 为假表示这只宠物的
// 舞台放不下(理论上不会走到, pet_atlas_load() 已经拦过一道)。
static pet_layout_t s_layout;
static bool          s_layout_ok;
static bool          s_pet_ready;   // 建界面时槽里有没有宠物

static lv_obj_t *s_screen;
static lv_obj_t *s_dot;
static lv_obj_t *s_status;
static lv_obj_t *s_bat_text;
static lv_obj_t *s_bat_fill;
static lv_obj_t *s_gauge_track[2];  // [0]=左槽(5 小时窗) [1]=右槽(周窗)
static lv_obj_t *s_gauge_fill[2];
static lv_obj_t *s_gauge_label[2];
static lv_obj_t *s_plat_body;
static lv_obj_t *s_plat_text;
static lv_obj_t *s_stage;
static lv_obj_t *s_sprite;
static lv_obj_t *s_sleep;
static lv_obj_t *s_stage_hint;   // 没有宠物时舞台区的占位文字

// 帧落在舞台外只可能是包坏了; 每帧都刷一条日志没有意义, 但一条不刷会让人
// 对着空屏幕猜。用一个"说过一次"的闩, 换宠物时复位。
static bool s_bad_frame_warned;

static lv_obj_t *s_info_scrim;
static lv_obj_t *s_info_panel;
static lv_obj_t *s_info_value[INFO_ROW_COUNT];

// 传输页: 与宠物界面互斥的另一块屏幕(见 pet_ui.h)。它不引用任何图集像素,
// 所以可以在图集解除映射之后继续显示。
static lv_obj_t *s_transfer_screen;
static lv_obj_t *s_transfer_name;
static lv_obj_t *s_transfer_msg;
static lv_obj_t *s_transfer_bar;
static lv_obj_t *s_transfer_fill;
static uint32_t  s_transfer_total;
static uint32_t  s_transfer_seen;
static uint32_t  s_transfer_tone = COL_MUTED;
static char      s_transfer_msg_text[64];

// 配网页: 一个不透明的全屏覆盖层。它盖住宠物舞台是故意的 —— 设备还没联网时
// 宠物本来就在睡觉, 这时候让配对码成为画面上唯一焦点更好读。
static lv_obj_t *s_prov_scrim;
static lv_obj_t *s_prov_name;
static lv_obj_t *s_prov_pin;
static lv_obj_t *s_prov_status;

static lv_timer_t *s_anim_timer;
// 屏幕黑着的时候不必逐帧重绘(见 pet_ui_set_anim_enabled)。默认渲染, 由 pet_app
// 在息屏/亮屏时改。**不放在 pet_ui_init 里复位**: 屏幕开关的真值在 pet_app, 这里
// 跟着走就行, 两处都记一份迟早对不上。
static bool s_anim_enabled = true;

static pet_state_t       s_state;
static pet_settings_t    s_settings;
static bool              s_settings_ok;
static int               s_battery;

// Codex 用量限额。存的是**已用**百分比; <0 表示还没有快照, 对应的槽要藏着。
// 与电量一样: build/destroy 只管对象, 数值留在模块里, 重建界面后立刻恢复。
static int s_limit_primary = -1;
static int s_limit_weekly  = -1;

static char     s_prov_device[24];
static char     s_prov_pin_text[8];
static char     s_prov_status_text[80];
static uint32_t s_prov_tone = COL_INK;
static bool     s_prov_visible;

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
    // 行距收紧(见 TEXT_LINE_SPACE): 只有换行/多行文本看得出差别。
    lv_obj_set_style_text_line_space(label, TEXT_LINE_SPACE, 0);
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

    // 没有宠物时, 站台上那句要说的是"怎么办", 而不是 Codex 在等什么 ——
    // 槽是空的, 只可能是用户还没装。
    const bool placeholder = (s_text[0] == '\0') || !s_pet_ready;
    if (placeholder) {
        lv_label_set_text(s_plat_text,
                          s_pet_ready ? placeholder_text(link, codex)
                                      : PET_STR_PH_NO_PET);
    }
    lv_obj_set_style_text_color(
        s_plat_text,
        lv_color_hex(s_text[0] != '\0' && s_pet_ready ? COL_INK : COL_MUTED), 0);

    if (s_sprite == NULL) return;   // 没有宠物: 舞台区只有占位文字, 没有可压暗的图像

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

// 能量槽: 按剩余百分比把填充从槽底顶上来。掉线时不显示 —— 那时的快照是旧的,
// 画上去就是骗人; 没有快照的窗口整组隐藏(轨道一起藏, 台身上不留两条没有意义
// 的空槽)。跑在持有 LVGL 锁的路径里。
static void refresh_gauges_locked(void)
{
    const bool online = s_state.link == PET_LINK_ONLINE;
    const int used[2] = { s_limit_primary, s_limit_weekly };
    const uint32_t color[2] = { COL_BAD, COL_ACCENT };
    // 槽的竖直范围: 台身顶留 VPAD; 底部依次让出标签行、一线间隔、圆角净空。
    // 台身几何是运行时值。
    const int track_top = s_layout.plat_body_y + GAUGE_VPAD;
    const int track_h = s_layout.plat_body_h - GAUGE_VPAD
                        - GAUGE_LABEL_H - GAUGE_LABEL_GAP - GAUGE_BOTTOM_CLEAR;
    const int track_x[2] = {
        s_layout.plat_x + GAUGE_INSET,
        s_layout.plat_x + s_layout.plat_w - GAUGE_INSET - GAUGE_W,
    };

    for (int i = 0; i < 2; i++) {
        if (s_gauge_track[i] == NULL || s_gauge_fill[i] == NULL) continue;

        if (!online || used[i] < 0 || used[i] > 100 || track_h < GAUGE_MIN_H) {
            lv_obj_add_flag(s_gauge_track[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_gauge_fill[i], LV_OBJ_FLAG_HIDDEN);
            if (s_gauge_label[i] != NULL) {
                lv_obj_add_flag(s_gauge_label[i], LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }

        lv_obj_set_size(s_gauge_track[i], GAUGE_W, track_h);
        lv_obj_set_pos(s_gauge_track[i], track_x[i], track_top);
        lv_obj_remove_flag(s_gauge_track[i], LV_OBJ_FLAG_HIDDEN);

        // 标签钉在槽底正下方: 左标签左对齐、右标签右对齐于各自的槽。
        if (s_gauge_label[i] != NULL) {
            lv_obj_set_size(s_gauge_label[i], GAUGE_LABEL_W, GAUGE_LABEL_H);
            lv_obj_set_pos(s_gauge_label[i],
                           i == 0 ? track_x[0]
                                  : track_x[1] + GAUGE_W - GAUGE_LABEL_W,
                           track_top + track_h + GAUGE_LABEL_GAP);
            lv_obj_remove_flag(s_gauge_label[i], LV_OBJ_FLAG_HIDDEN);
        }

        const int remain = 100 - used[i];
        int height = (remain * track_h) / 100;
        if (height < GAUGE_MIN_H) height = GAUGE_MIN_H;

        // 底对齐: 填充永远从槽底往上生长, "能量还剩多少"一眼可比。
        lv_obj_set_size(s_gauge_fill[i], GAUGE_W, height);
        lv_obj_set_pos(s_gauge_fill[i], track_x[i], track_top + track_h - height);
        lv_obj_set_style_bg_color(s_gauge_fill[i], lv_color_hex(color[i]), 0);
        lv_obj_set_style_bg_opa(s_gauge_fill[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_gauge_fill[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// 信息面板里的动态行: 链路 / 电量。
static void refresh_info_locked(void)
{
    if (s_info_panel == NULL) return;

    lv_label_set_text(s_info_value[INFO_ROW_LINK],
                      status_text(s_state.link, s_state.codex));

    lv_label_set_text(s_info_value[INFO_ROW_FIRMWARE], PET_FIRMWARE_VERSION);
    // 宠物不再是编译期常量, 所以这一行报的是槽里**当前**那只的 id(没有则 none),
    // 与 hello 报文里 pet 字段同源 —— 换完宠物不用重刷固件就能看到这里变了。
    lv_label_set_text(s_info_value[INFO_ROW_PACKAGE], pet_atlas_pet_id());
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
    if (s_sprite == NULL) return;   // 没有宠物

    const uint16_t count = pet_atlas_frame_count(s_playing);
    if (count == 0) return;
    if (s_frame >= count) s_frame = 0;

    const pet_pkg_frame_t *frame = pet_atlas_frame(s_playing, s_frame);
    const lv_image_dsc_t *image = pet_atlas_image(s_playing, s_frame);
    if (frame == NULL || image == NULL) return;

    lv_image_set_src(s_sprite, image);

    // 关键: 每帧在图集单元格里的裁剪原点不同, 扣掉**舞台在单元格里的裁剪原点**
    // 才是它在舞台里的落点。动作里的位移(跑动、跳跃的横向移动)就是这么来的 ——
    // 与 ChatGPT 一致, 不是我们额外加的补间。
    //
    // 基准必须取 pet_atlas_stage_x/y()(包头的 stage_x/stage_y, 即所有帧裁剪框的
    // 并集原点)。**不能取 s_layout.stage_x/y** —— 那是舞台上屏幕的落点, 在另一个
    // 坐标系里(129 px 宽的舞台: 单元格原点 31, 屏幕落点 55), 拿它来减会让整个宠物
    // 左上偏移 24/33 px 并被舞台裁掉一角。换算与这道判断都在 pet_layout.c 里,
    // 主机测试逐条钉住(tests/test_pet_layout.c)。
    pet_layout_rect_t rect;
    if (!pet_layout_frame_rect(frame->x, frame->y, frame->w, frame->h,
                               (int)pet_atlas_stage_x(),
                               (int)pet_atlas_stage_y(),
                               &s_layout, &rect)) {
        if (!s_bad_frame_warned) {
            s_bad_frame_warned = true;
            ESP_LOGW(TAG, "帧 %u %dx%d @ (%d,%d) 落在舞台 %dx%d @ (%d,%d) 外, 不画",
                     (unsigned)s_frame, frame->w, frame->h, frame->x, frame->y,
                     s_layout.stage_w, s_layout.stage_h, s_layout.stage_x,
                     s_layout.stage_y);
        }
        return;
    }
    lv_obj_set_pos(s_sprite, rect.x, rect.y);

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
    // 配网页是不透明的, 底下的宠物没人看得见 —— 别再逐帧重绘它, 省 CPU 也省电。
    // 息屏同理(屏幕整个黑着)。槽里没有宠物时也没有帧可画, 醒着只是空转。
    if (s_screen == NULL || s_prov_visible || !s_pet_ready || !s_anim_enabled) {
        return;
    }

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

static void build_gauges(void)
{
    // 台身(文字框)内左右两条竖形能量槽, 显示 Codex 用量的**剩余**量。
    // 轨道 = 深色暗条, 标出"满格"的高度; 填充对象单独记下来, 高度/颜色由
    // refresh_gauges_locked() 按 s_layout 与收到的快照算 —— 这里不摆坐标。
    // 没数据之前整组隐藏, 别让台身上挂两条没有意义的空槽。
    static const uint32_t color[2] = { COL_BAD, COL_ACCENT };
    static const char *const labels[2] = {
        PET_STR_LIMIT_PRIMARY, PET_STR_LIMIT_WEEKLY,
    };

    for (int i = 0; i < 2; i++) {
        s_gauge_track[i] = make_box(s_screen, 0, 0, 1, 1, COL_CARD, 2);
        lv_obj_set_style_bg_opa(s_gauge_track[i], LV_OPA_60, 0);
        lv_obj_add_flag(s_gauge_track[i], LV_OBJ_FLAG_HIDDEN);

        s_gauge_fill[i] = make_box(s_screen, 0, 0, 1, 1, color[i], 2);
        lv_obj_set_style_bg_opa(s_gauge_fill[i], LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_gauge_fill[i], LV_OBJ_FLAG_HIDDEN);

        // 行框宽是固定的, 对齐方式决定文字贴槽的哪一侧。
        // 行框只钉宽不钉高: 高度钉死会把字库真实行高(>16)裁出"下半截缺失"。
        // montserrat_14 是 LVGL 内置字体(sdkconfig 已启用), 行高恰好 16,
        // 与 GAUGE_LABEL_H 的几何预留吻合; ASCII 覆盖足够两条小标签。
        s_gauge_label[i] = make_label(s_screen, labels[i], &lv_font_montserrat_14,
                                      COL_MUTED);
        lv_obj_set_width(s_gauge_label[i], GAUGE_LABEL_W);
        lv_obj_set_style_text_align(s_gauge_label[i],
                                    i == 0 ? LV_TEXT_ALIGN_LEFT
                                           : LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(s_gauge_label[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_stage(void)
{
    // 舞台不画任何东西, 只负责把宠物夹在自己的范围里, 并给帧位置一个原点。
    // 宠物身下不留底色: 多一层浅色卡片会把轮廓框住, 动作幅度反而看不清;
    // "地面"由站台和脚底阴影交代, 见 build_platform()。
    s_stage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_stage);
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_stage, s_layout.stage_x, s_layout.stage_y);
    lv_obj_set_size(s_stage, s_layout.stage_w, s_layout.stage_h);
    lv_obj_set_style_bg_opa(s_stage, LV_OPA_TRANSP, 0);

    if (!s_pet_ready) {
        // 槽是空的: 舞台区放一句占位, 免得屏幕上半截完全空白, 看起来像没启动。
        // 位置由 lv_obj_center 算, 不写死坐标 —— 舞台尺寸是运行时才知道的。
        s_stage_hint = make_label(s_stage, PET_STR_NO_PET, pet_font_body(),
                                  COL_MUTED);
        lv_obj_center(s_stage_hint);
        return;
    }

    s_sprite = lv_image_create(s_stage);
    lv_obj_remove_style_all(s_sprite);
    lv_obj_set_pos(s_sprite, 0, 0);

    // 睡眠标记浮在宠物右上方那块空白上(素材头部只占左半边)。
    s_sleep = make_label(s_stage, PET_STR_SLEEP_MARK, pet_font_body(),
                         COL_ACCENT);
    lv_obj_set_pos(s_sleep, s_layout.sleep_x, s_layout.sleep_y);
    lv_obj_set_style_text_opa(s_sleep, LV_OPA_70, 0);
}

// 站台: 台身(深) + 台面(浅) + 台面顶沿高光 + 脚底接触阴影, 四层叠出"一块板"。
// 全部建在舞台之前 —— LVGL 按创建顺序决定叠放, 宠物必须压在台面之上,
// 否则脚会被台面盖掉, 变成"站在台子后面"。
static void build_platform(void)
{
    s_plat_body = make_box(s_screen, s_layout.plat_x, s_layout.plat_body_y,
                           s_layout.plat_w, s_layout.plat_body_h, COL_PLAT_BODY,
                           s_layout.plat_radius);
    lv_obj_set_style_border_width(s_plat_body, 1, 0);
    lv_obj_set_style_border_color(s_plat_body, lv_color_hex(COL_CARD_EDGE), 0);

    // 台面: 竖直渐变让上沿亮、下沿并入台身, 接缝不生硬。
    lv_obj_t *floor = make_box(s_screen, s_layout.plat_x, s_layout.plat_floor_y,
                               s_layout.plat_w, s_layout.plat_floor_h,
                               COL_PLAT_FLOOR, s_layout.plat_floor_radius);
    lv_obj_set_style_bg_grad_dir(floor, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(floor, lv_color_hex(COL_PLAT_BODY), 0);

    // 顶沿高光: 只留中间一段, 两端收进去, 避免和台面圆角打架。
    make_box(s_screen, s_layout.plat_x + s_layout.plat_rim_inset,
             s_layout.plat_floor_y,
             s_layout.plat_w - s_layout.plat_rim_inset * 2, s_layout.plat_rim_h,
             COL_PLAT_RIM, s_layout.plat_rim_h / 2);

    // 接触阴影: 半透明黑压在台面上, 位置取自脚底正下方。跳跃动作抬起时
    // 阴影不动 —— 它是"地面", 不是宠物的挂件。
    lv_obj_t *shadow = make_box(s_screen, PET_LAYOUT_SCR_W / 2 - s_layout.shadow_w / 2,
                                s_layout.shadow_y, s_layout.shadow_w,
                                s_layout.shadow_h, 0x000000,
                                s_layout.shadow_h / 2);
    lv_obj_set_style_bg_opa(shadow, LV_OPA_40, 0);

    // 文字落在台身上: 台身就是原来的气泡, 只是换了块"水泥"。
    const lv_font_t *font = pet_font_body();
    const int text_h = font->line_height * 2;
    s_plat_text = make_label(s_screen, "", font, COL_MUTED);
    lv_obj_set_size(s_plat_text, s_layout.plat_text_w, text_h);
    lv_label_set_long_mode(s_plat_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_plat_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_plat_text, s_layout.plat_text_x, s_layout.plat_text_y);
}

static void build_info(void)
{
    s_info_scrim = make_box(s_screen, 0, 0, PET_LAYOUT_SCR_W, PET_LAYOUT_SCR_H,
                            0x000000, 0);
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

    // 三条 hint(-44/-26/-8, 间距 18)。16px 字库的字形只占行框 [top+7, top+16],
    // 逐条算: 数据区末行字形底 = 48+5*24+16 = 184; hint_dim 字形 [191,200];
    // hint_close [209,218]; hint_demo [227,236] —— 相邻都不碰。改动前先重算。
    lv_obj_t *hint_dim = make_label(s_info_panel, PET_STR_INFO_HINT_DIM,
                                    pet_font_body(), COL_MUTED);
    lv_obj_align(hint_dim, LV_ALIGN_BOTTOM_MID, 0, -44);

    lv_obj_t *hint_close = make_label(s_info_panel, PET_STR_INFO_HINT_CLOSE,
                                      pet_font_body(), COL_MUTED);
    lv_obj_align(hint_close, LV_ALIGN_BOTTOM_MID, 0, -26);

    lv_obj_t *hint_demo = make_label(s_info_panel, PET_STR_INFO_HINT_DEMO,
                                     pet_font_body(), COL_MUTED);
    lv_obj_align(hint_demo, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_add_flag(s_info_scrim, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// 蓝牙配网页
// ---------------------------------------------------------------------------
static void build_provision(void)
{
    s_prov_scrim = make_box(s_screen, 0, 0, PET_LAYOUT_SCR_W, PET_LAYOUT_SCR_H,
                            COL_BG, 0);

    lv_obj_t *title = make_label(s_prov_scrim, PET_STR_PROV_TITLE,
                                 pet_font_title(), COL_INK);
    lv_obj_set_width(title, PET_LAYOUT_SCR_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, PROV_TITLE_Y);

    // 设备名要让用户能和 Mac 上扫到的那个对上, 否则家里有两个同类设备时不知道
    // 该连哪个。
    s_prov_name = make_label(s_prov_scrim, "", pet_font_body(), COL_MUTED);
    lv_obj_set_width(s_prov_name, PET_LAYOUT_SCR_W);
    lv_obj_set_style_text_align(s_prov_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_prov_name, 0, PROV_NAME_Y);

    lv_obj_t *card = make_box(s_prov_scrim, PROV_CARD_X, PROV_CARD_Y,
                              PROV_CARD_W, PROV_CARD_H, COL_CARD, 16);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT), 0);

    lv_obj_t *pin_label = make_label(card, PET_STR_PROV_PIN_LABEL,
                                     pet_font_body(), COL_MUTED);
    lv_obj_set_width(pin_label, PROV_CARD_W);
    lv_obj_set_style_text_align(pin_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(pin_label, 0, PROV_PIN_LABEL_Y);

    s_prov_pin = make_label(card, "", pet_font_title(), COL_ACCENT);
    lv_obj_set_width(s_prov_pin, PROV_CARD_W);
    lv_obj_set_style_text_align(s_prov_pin, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_prov_pin, 0, PROV_PIN_Y);

    // 状态文字会随流程变长变短(带 IP 的那条最长), 留两行并允许换行。
    s_prov_status = make_label(s_prov_scrim, "", pet_font_body(), COL_INK);
    lv_obj_set_size(s_prov_status, PROV_STATUS_W, PROV_STATUS_H);
    lv_label_set_long_mode(s_prov_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_prov_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_prov_status, PROV_STATUS_X, PROV_STATUS_Y);

    lv_obj_t *hint = make_label(s_prov_scrim, PET_STR_PROV_HINT,
                                pet_font_body(), COL_MUTED);
    lv_obj_set_width(hint, PET_LAYOUT_SCR_W);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 0, PROV_HINT_Y);

    lv_obj_add_flag(s_prov_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_provision_locked(void)
{
    if (s_prov_scrim == NULL) return;

    // 20 px 的字体下 "1234" 四个数字挤在一起念不清, 拉开成一格一个。
    char spaced[16];
    if (strlen(s_prov_pin_text) == 4) {
        snprintf(spaced, sizeof(spaced), "%c %c %c %c", s_prov_pin_text[0],
                 s_prov_pin_text[1], s_prov_pin_text[2], s_prov_pin_text[3]);
    } else {
        snprintf(spaced, sizeof(spaced), "- - - -");
    }

    lv_label_set_text(s_prov_name, s_prov_device);
    lv_label_set_text(s_prov_pin, spaced);
    lv_label_set_text(s_prov_status, s_prov_status_text);
    lv_obj_set_style_text_color(s_prov_status, lv_color_hex(s_prov_tone), 0);
}

// 动画定时器的周期只由这一处决定。要逐帧渲染时置 1 —— 下一拍立刻重新装载动作
// (frame 的真实时长随后由 render_frame 按素材写回); 不需要渲染时拉长到 500, 让
// 定时器空转的代价也接近零。
//
// 之所以收成一处: 周期有三个来源(配网页显隐、息屏/亮屏、重建界面), 各写各的话,
// 最后一个写的赢, 而"谁最后写的"在运行期根本看不出来。
static void apply_anim_period_locked(void)
{
    if (s_anim_timer == NULL) return;

    const bool render = s_anim_enabled && !s_prov_visible && s_pet_ready
                        && s_screen != NULL;
    lv_timer_set_period(s_anim_timer, render ? 1 : 500);
    lv_timer_reset(s_anim_timer);
}

static void set_provision_visible_locked(bool visible)
{
    s_prov_visible = visible;
    if (s_prov_scrim == NULL) return;

    if (visible) {
        lv_obj_move_foreground(s_prov_scrim);
        lv_obj_remove_flag(s_prov_scrim, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_prov_scrim, LV_OBJ_FLAG_HIDDEN);
    }

    // 配网页打开时底下的宠物不用再逐帧渲染, 关掉时下一拍要立刻重新装载动作。
    apply_anim_period_locked();
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

    // 换宠物的流程是「拆界面 -> 换包 -> 重建」, 所以这里可能是从传输页回来的:
    // 先把传输页收掉, 免得两块钱屏幕叠着。
    if (s_transfer_screen != NULL) {
        lv_obj_delete(s_transfer_screen);
        s_transfer_screen = NULL;
        s_transfer_name = NULL;
        s_transfer_msg = NULL;
        s_transfer_bar = NULL;
        s_transfer_fill = NULL;
    }

    // 舞台尺寸由当前宠物决定; 槽里没有宠物时用参照尺寸占位, 这样空槽的画面与
    // 装了参照宠物时版式完全一致。
    s_pet_ready = pet_atlas_ready();
    int stage_w = s_pet_ready ? (int)pet_atlas_stage_width()
                              : PET_LAYOUT_STAGE_W_REF;
    int stage_h = s_pet_ready ? (int)pet_atlas_stage_height()
                              : PET_LAYOUT_STAGE_H_REF;
    s_layout_ok = pet_layout_for_stage(stage_w, stage_h, &s_layout);
    if (!s_layout_ok) {
        // pet_atlas_load() 已经按同一个判据拦过一次, 走到这里说明是内部不一致。
        ESP_LOGE(TAG, "舞台 %dx%d 算不出可用版式", stage_w, stage_h);
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
    lv_obj_set_size(s_screen, PET_LAYOUT_SCR_W, PET_LAYOUT_SCR_H);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_screen_load(s_screen);

    build_top_row();
    build_platform();   // 先于舞台: 宠物要压在台面之上, 否则脚会被盖住
    build_stage();
    // 能量槽最后建(信息面板之前): 它画在台身正面, 必须压在站台之上 —— LVGL
    // 后建的兄弟对象盖先建的, 摆在 build_platform() 之前会被台身整个盖住。
    build_gauges();
    build_info();
    build_provision();   // 最后建: 配网页要盖在信息面板之上
    refresh_status_locked();
    refresh_battery_locked();
    refresh_gauges_locked();
    refresh_provision_locked();
    set_info_visible_locked(s_info_visible);  // 重建时恢复面板显隐

    // 开机第一拍: 让定时器把第 0 帧画出来。
    s_anim_timer = lv_timer_create(anim_tick, 1, NULL);
    // 放在定时器之后: 里面要按显隐调定时器周期。
    set_provision_visible_locked(s_prov_visible);

    ESP_LOGI(TAG, "界面就绪: 宠物 %s, 舞台 %dx%d @ (%d,%d) 脚底 y=%d, "
                  "站台 %d..%d, 行高 %d",
             pet_atlas_pet_id(), s_layout.stage_w, s_layout.stage_h,
             s_layout.stage_x, s_layout.stage_y, s_layout.plat_floor_y,
             s_layout.plat_floor_y, s_layout.plat_body_y + s_layout.plat_body_h,
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
    s_prov_visible = false;
    s_prov_device[0] = '\0';
    s_prov_pin_text[0] = '\0';
    s_prov_status_text[0] = '\0';
    s_prov_tone = COL_INK;
    // 版式由 pet_ui_build() 算; 在这里先置成"还没有", 免得初始状态被误读成有宠物。
    s_pet_ready = false;
    s_layout_ok = false;
    s_bad_frame_warned = false;
    s_transfer_msg_text[0] = '\0';
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
    s_stage_hint = NULL;
    s_plat_body = NULL;
    s_plat_text = NULL;
    s_dot = NULL;
    s_status = NULL;
    s_bat_text = NULL;
    s_bat_fill = NULL;
    for (int i = 0; i < 2; i++) {
        s_gauge_fill[i] = NULL;
        s_gauge_track[i] = NULL;
        s_gauge_label[i] = NULL;
    }
    s_sleep = NULL;
    s_info_scrim = NULL;
    s_info_panel = NULL;
    for (int i = 0; i < INFO_ROW_COUNT; i++) s_info_value[i] = NULL;
    s_prov_scrim = NULL;
    s_prov_name = NULL;
    s_prov_pin = NULL;
    s_prov_status = NULL;
    s_playing = PET_ANIM_COUNT;
    s_frame = 0;
    s_override = false;

    bsp_lvgl_unlock();
}

bool pet_ui_ready(void)
{
    return s_screen != NULL;
}

void pet_ui_set_anim_enabled(bool enabled)
{
    if (!bsp_lvgl_lock(1000)) return;

    if (s_anim_enabled != enabled) {
        s_anim_enabled = enabled;
        apply_anim_period_locked();
    }

    bsp_lvgl_unlock();
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
        // 掉线时能量槽要藏起来(快照不再可信), 恢复时再按缓存的值放回去。
        refresh_gauges_locked();
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

void pet_ui_set_limits(int primary_used, int weekly_used)
{
    // 越界值按"没有快照"处理: 协议层拦过一道, 这里只兜内部不一致。
    if (primary_used < 0 || primary_used > 100) primary_used = -1;
    if (weekly_used < 0 || weekly_used > 100) weekly_used = -1;

    if (!bsp_lvgl_lock(1000)) return;
    s_limit_primary = primary_used;
    s_limit_weekly = weekly_used;
    refresh_gauges_locked();
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

void pet_ui_set_provision_visible(bool visible)
{
    if (!bsp_lvgl_lock(1000)) return;
    set_provision_visible_locked(visible);
    bsp_lvgl_unlock();
}

void pet_ui_set_provision(const char *device_name, const char *pin,
                          const char *status, pet_tone_t tone)
{
    if (!bsp_lvgl_lock(1000)) return;

    snprintf(s_prov_device, sizeof(s_prov_device), "%s",
             device_name != NULL ? device_name : "");
    snprintf(s_prov_pin_text, sizeof(s_prov_pin_text), "%s", pin != NULL ? pin : "");
    snprintf(s_prov_status_text, sizeof(s_prov_status_text), "%s",
             status != NULL ? status : "");

    switch (tone) {
    case PET_TONE_GOOD: s_prov_tone = COL_GOOD; break;
    case PET_TONE_BAD:  s_prov_tone = COL_BAD;  break;
    case PET_TONE_INFO:
    default:            s_prov_tone = COL_INK;  break;
    }

    refresh_provision_locked();
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

// ---------------------------------------------------------------------------
// 宠物传输页
// ---------------------------------------------------------------------------
// 为什么必须先把宠物界面拆掉: 图集是从 flash 上 mmap 出来的, 而换宠物要擦写同一
// 块 flash —— 一边映射一边擦是未定义行为。宠物界面上的 s_sprite 正引用着那块内存
// 里的图像描述符, 所以解除映射之前必须让这些对象先消失。
//
// 拆掉之后屏幕不能是黑的: 一份 1~3 MB 的包在 2.4 GHz Wi-Fi 上要传十几秒到几十秒,
// 期间用户完全看不出设备在不在干活, 拔电就只剩半份包。所以这里顶上一块只有文字和
// 进度条的屏幕。
//
// 这块屏幕上的一切都不引用图集, 所以它能在图集解除映射之后继续显示。
static void build_transfer_locked(const char *pet_id)
{
    s_transfer_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_transfer_screen);
    lv_obj_set_size(s_transfer_screen, PET_LAYOUT_SCR_W, PET_LAYOUT_SCR_H);
    lv_obj_set_style_bg_color(s_transfer_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_transfer_screen, LV_OPA_COVER, 0);
    lv_screen_load(s_transfer_screen);

    lv_obj_t *title = make_label(s_transfer_screen, PET_STR_TRANSFER_TITLE,
                                 pet_font_title(), COL_INK);
    lv_obj_set_width(title, PET_LAYOUT_SCR_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, TRANS_TITLE_Y);

    // 宠物 id 是 ASCII 且最长 31 字节, 一行放不下就用省略号收尾 —— 它是这里唯一
    // 能让用户确认"发的是哪一只"的信息, 不写成固定宽度的框, 免得撑破版式。
    s_transfer_name = make_label(s_transfer_screen, pet_id != NULL ? pet_id : "",
                                 pet_font_body(), COL_ACCENT);
    lv_obj_set_width(s_transfer_name, PET_LAYOUT_SCR_W - 2 * TRANS_MSG_X);
    lv_label_set_long_mode(s_transfer_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_transfer_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_transfer_name, TRANS_MSG_X, TRANS_NAME_Y);

    // 进度条: 外框 + 填充。填充宽度按收到的比例算, 与顶栏电量条同一个套路。
    lv_obj_t *track = make_box(s_transfer_screen, TRANS_BAR_X, TRANS_BAR_Y,
                               TRANS_BAR_W, TRANS_BAR_H, COL_CARD, TRANS_BAR_H / 2);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, lv_color_hex(COL_CARD_EDGE), 0);

    s_transfer_bar = track;
    s_transfer_fill = make_box(track, 1, 1, 1, TRANS_BAR_H - 2, COL_ACCENT,
                               (TRANS_BAR_H - 2) / 2);
    lv_obj_set_style_bg_opa(s_transfer_fill, LV_OPA_TRANSP, 0);

    s_transfer_msg = make_label(s_transfer_screen, "", pet_font_body(), COL_MUTED);
    lv_obj_set_size(s_transfer_msg, TRANS_MSG_W, TRANS_MSG_H);
    lv_label_set_long_mode(s_transfer_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_transfer_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_transfer_msg, TRANS_MSG_X, TRANS_MSG_Y);

    lv_label_set_text(s_transfer_msg, s_transfer_msg_text);
    lv_obj_set_style_text_color(s_transfer_msg, lv_color_hex(s_transfer_tone), 0);
}

static void refresh_transfer_locked(void)
{
    if (s_transfer_msg == NULL) return;

    lv_label_set_text(s_transfer_msg, s_transfer_msg_text);
    lv_obj_set_style_text_color(s_transfer_msg, lv_color_hex(s_transfer_tone), 0);

    if (s_transfer_fill == NULL) return;
    if (s_transfer_total == 0) return;   // 长度未知: 只让外框显示, 不假装有进度

    uint32_t percent = (uint32_t)((uint64_t)s_transfer_seen * 100 / s_transfer_total);
    if (percent > 100) percent = 100;
    int width = (int)((TRANS_BAR_W - 2) * percent / 100);
    if (width < 1) width = 1;
    lv_obj_set_size(s_transfer_fill, width, TRANS_BAR_H - 2);
    lv_obj_set_style_bg_opa(s_transfer_fill, LV_OPA_COVER, 0);
}

void pet_ui_begin_transfer(const char *pet_id, uint32_t total_bytes)
{
    if (!bsp_lvgl_lock(1000)) return;

    // 先拆宠物界面 —— 顺序不能反: 对象还在的时候解除映射就是野指针。
    if (s_anim_timer != NULL) {
        lv_timer_delete(s_anim_timer);
        s_anim_timer = NULL;
    }
    if (s_screen != NULL) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_sprite = NULL;
    s_stage = NULL;
    s_stage_hint = NULL;
    s_plat_body = NULL;
    s_plat_text = NULL;
    s_dot = NULL;
    s_status = NULL;
    s_bat_text = NULL;
    s_bat_fill = NULL;
    for (int i = 0; i < 2; i++) {
        s_gauge_fill[i] = NULL;
        s_gauge_track[i] = NULL;
        s_gauge_label[i] = NULL;
    }
    s_sleep = NULL;
    s_info_scrim = NULL;
    s_info_panel = NULL;
    for (int i = 0; i < INFO_ROW_COUNT; i++) s_info_value[i] = NULL;
    s_prov_scrim = NULL;
    s_prov_name = NULL;
    s_prov_pin = NULL;
    s_prov_status = NULL;
    s_prov_visible = false;
    s_playing = PET_ANIM_COUNT;
    s_frame = 0;
    s_override = false;

    s_transfer_total = total_bytes;
    s_transfer_seen = 0;
    s_transfer_tone = COL_MUTED;
    snprintf(s_transfer_msg_text, sizeof(s_transfer_msg_text), "%s",
             PET_STR_TRANSFER_ENABLING);

    if (s_transfer_screen != NULL) {
        lv_obj_delete(s_transfer_screen);
        s_transfer_screen = NULL;
    }
    build_transfer_locked(pet_id);
    refresh_transfer_locked();

    ESP_LOGI(TAG, "传输页: %s, 声明 %u 字节", pet_id != NULL ? pet_id : "?",
             (unsigned)total_bytes);
    bsp_lvgl_unlock();
}

void pet_ui_set_transfer_progress(uint32_t received_bytes)
{
    if (!bsp_lvgl_lock(1000)) return;
    if (s_transfer_screen == NULL) {
        bsp_lvgl_unlock();
        return;
    }
    s_transfer_seen = received_bytes;
    refresh_transfer_locked();
    bsp_lvgl_unlock();
}

void pet_ui_set_transfer_message(const char *text, pet_tone_t tone)
{
    if (!bsp_lvgl_lock(1000)) return;

    snprintf(s_transfer_msg_text, sizeof(s_transfer_msg_text), "%s",
             text != NULL ? text : "");
    switch (tone) {
    case PET_TONE_GOOD: s_transfer_tone = COL_GOOD; break;
    case PET_TONE_BAD:  s_transfer_tone = COL_BAD;  break;
    case PET_TONE_INFO:
    default:            s_transfer_tone = COL_INK;  break;
    }

    refresh_transfer_locked();
    bsp_lvgl_unlock();
}

void pet_ui_end_transfer(void)
{
    if (!bsp_lvgl_lock(1000)) return;

    if (s_transfer_screen != NULL) {
        lv_obj_delete(s_transfer_screen);
        s_transfer_screen = NULL;
    }
    s_transfer_name = NULL;
    s_transfer_msg = NULL;
    s_transfer_bar = NULL;
    s_transfer_fill = NULL;

    bsp_lvgl_unlock();
}

bool pet_ui_transferring(void)
{
    return s_transfer_screen != NULL;
}
