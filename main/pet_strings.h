// main/pet_strings.h
// 界面文案的唯一来源。
//
// 这里先定义代码里直接引用的宏, 再用同一批字面量组成 PET_UI_STRING_LIST。
// 好处是「界面上显示的文案」和「测试要核对的文案」共用一份字面量, 不会各改各的。
//
// 中文界面必须自备字库(基线只带 Montserrat, 没有任何汉字), 覆盖情况由
//   - 启动自检 pet_app.c: 逐个查 glyph, 缺字直接打日志
//   - 主机测试 tests/test_pet_font_coverage.py: 对照 assets/fonts/pet_font_charset.json
// 两道关卡保证。新增文案时只需改本文件, 两处都会自动跟上。
#pragma once

// ---------------------------------------------------------------------------
// 状态标签(20 px)
// ---------------------------------------------------------------------------
#define PET_STR_STATUS_OFFLINE     "离线"
#define PET_STR_STATUS_CONNECTING  "连接中"
#define PET_STR_STATUS_IDLE        "空闲"
#define PET_STR_STATUS_WORKING     "工作中"
#define PET_STR_STATUS_WAITING     "等待确认"
#define PET_STR_STATUS_READY       "已完成"
#define PET_STR_STATUS_FAILED      "出错了"

// ---------------------------------------------------------------------------
// 气泡占位文案(16 px)。仅在 Bridge 没有下发文本时显示。
// ---------------------------------------------------------------------------
#define PET_STR_PH_OFFLINE         "未连接 Codex"
#define PET_STR_PH_CONNECTING      "正在连接配对端"
#define PET_STR_PH_IDLE            "等待 Codex 任务"
#define PET_STR_PH_WORKING         "Codex 正在执行"
#define PET_STR_PH_WAITING         "需要你确认"
#define PET_STR_PH_READY           "任务已完成"
#define PET_STR_PH_FAILED          "任务中断"

// ---------------------------------------------------------------------------
// 信息面板(标题 20 px, 正文 16 px)
// ---------------------------------------------------------------------------
#define PET_STR_INFO_TITLE         "Codex 宠物"
#define PET_STR_INFO_FIRMWARE      "固件版本"
#define PET_STR_INFO_PACKAGE       "宠物包"
#define PET_STR_INFO_LINK          "链路"
#define PET_STR_INFO_WIFI          "网络"
#define PET_STR_INFO_BRIDGE        "配对端"
#define PET_STR_INFO_BATTERY       "电量"
#define PET_STR_INFO_UNKNOWN       "未知"
#define PET_STR_INFO_HINT_CLOSE    "长按确定键关闭"
#define PET_STR_INFO_HINT_DEMO     "长按上键进入演示菜单"

// 睡眠标记, 使用拉丁字母, 走 fallback 字体。
#define PET_STR_SLEEP_MARK         "Zzz"

// ---------------------------------------------------------------------------
// 蓝牙配网页(标题 20 px, 正文 16 px)
// ---------------------------------------------------------------------------
#define PET_STR_PROV_TITLE         "蓝牙配网"
#define PET_STR_PROV_PIN_LABEL     "配对码"
// 状态区宽 PROV_STATUS_W=200px, 文案要留在单行内 —— 换行会让整屏看起来失焦。
// 这两条原先分别是 207px / 205px, 正好压线换行。改文案时用
// tests/test_pet_ui_text_fit.py 量一下, 别凭感觉。
#define PET_STR_PROV_WAITING       "Mac 上打开「配置 Wi-Fi」"
#define PET_STR_PROV_CONNECTED     "已连接，请输入配对码"
#define PET_STR_PROV_PAIRED        "已配对，正在接收参数"
#define PET_STR_PROV_APPLYING      "正在连接 Wi-Fi…"
#define PET_STR_PROV_SAVING        "正在保存…"
#define PET_STR_PROV_DONE          "配网完成"
#define PET_STR_PROV_FAILED        "配网失败"
#define PET_STR_PROV_ERR_SAVE      "保存参数失败"
#define PET_STR_PROV_ERR_CONNECT   "无法连接到这个网络"
#define PET_STR_PROV_HINT          "长按下键退出配网"
#define PET_STR_PROV_HINT_OPEN     "长按下键配网"

// ---------------------------------------------------------------------------
// 文案清单。X 宏展开, 每个条目 = (键, 字面量, 界面上使用的字号)
// ---------------------------------------------------------------------------
#define PET_UI_STRING_LIST(X)                                  \
    X(status_offline,    PET_STR_STATUS_OFFLINE,    20)        \
    X(status_connecting, PET_STR_STATUS_CONNECTING, 20)        \
    X(status_idle,       PET_STR_STATUS_IDLE,       20)        \
    X(status_working,    PET_STR_STATUS_WORKING,    20)        \
    X(status_waiting,    PET_STR_STATUS_WAITING,    20)        \
    X(status_ready,      PET_STR_STATUS_READY,      20)        \
    X(status_failed,     PET_STR_STATUS_FAILED,     20)        \
    X(ph_offline,        PET_STR_PH_OFFLINE,        16)        \
    X(ph_connecting,     PET_STR_PH_CONNECTING,     16)        \
    X(ph_idle,           PET_STR_PH_IDLE,           16)        \
    X(ph_working,        PET_STR_PH_WORKING,        16)        \
    X(ph_waiting,        PET_STR_PH_WAITING,        16)        \
    X(ph_ready,          PET_STR_PH_READY,          16)        \
    X(ph_failed,         PET_STR_PH_FAILED,         16)        \
    X(info_title,        PET_STR_INFO_TITLE,        20)        \
    X(info_firmware,     PET_STR_INFO_FIRMWARE,     16)        \
    X(info_package,      PET_STR_INFO_PACKAGE,      16)        \
    X(info_link,         PET_STR_INFO_LINK,         16)        \
    X(info_wifi,         PET_STR_INFO_WIFI,         16)        \
    X(info_bridge,       PET_STR_INFO_BRIDGE,       16)        \
    X(info_battery,      PET_STR_INFO_BATTERY,      16)        \
    X(info_unknown,      PET_STR_INFO_UNKNOWN,      16)        \
    X(info_hint_close,   PET_STR_INFO_HINT_CLOSE,   16)        \
    X(info_hint_demo,    PET_STR_INFO_HINT_DEMO,    16)        \
    X(sleep_mark,        PET_STR_SLEEP_MARK,        16)        \
    X(prov_title,        PET_STR_PROV_TITLE,        20)        \
    X(prov_pin_label,    PET_STR_PROV_PIN_LABEL,    16)        \
    X(prov_waiting,      PET_STR_PROV_WAITING,      16)        \
    X(prov_connected,    PET_STR_PROV_CONNECTED,    16)        \
    X(prov_paired,       PET_STR_PROV_PAIRED,       16)        \
    X(prov_applying,     PET_STR_PROV_APPLYING,     16)        \
    X(prov_saving,       PET_STR_PROV_SAVING,       16)        \
    X(prov_done,         PET_STR_PROV_DONE,         16)        \
    X(prov_failed,       PET_STR_PROV_FAILED,       16)        \
    X(prov_err_save,     PET_STR_PROV_ERR_SAVE,     16)        \
    X(prov_err_connect,  PET_STR_PROV_ERR_CONNECT,  16)        \
    X(prov_hint,         PET_STR_PROV_HINT,         16)        \
    X(prov_hint_open,    PET_STR_PROV_HINT_OPEN,    16)

typedef struct {
    const char   *key;   // 用于测试报告定位问题
    const char   *text;  // UTF-8 字面量
    unsigned char px;    // 16 或 20, 对应 pet_font_16 / pet_font_20
} pet_string_t;

extern const pet_string_t PET_UI_STRINGS[];
extern const unsigned      PET_UI_STRINGS_COUNT;
