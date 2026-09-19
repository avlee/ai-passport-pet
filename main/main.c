// main/main.c —— FoloToy AI Passport / Codex 宠物 固件入口。
//
// 只做三件事: 初始化 I2C 与显示/LVGL, 然后交给 pet_app。
// 其余逻辑按职责拆在:
//   pet_app.c     应用编排(按键、电量轮询、Bridge 事件)
//   pet_ui.c      宠物界面与动画驱动
//   pet_state.c   链路/Codex 状态 -> 动画行的映射(纯逻辑, 可主机测试)
//   pet_protocol.c线协议解析(纯逻辑, 可主机测试)
//   pet_bridge.c  Wi-Fi STA + TCP 客户端 + 重连
//   demo_menu.c   BSP 驱动参考示例(从宠物界面的信息面板长按上键进入)
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"  // 错误日志里要打印 BSP_LCD_* 引脚号
#include "esp_log.h"
#include "esp_sleep.h"
#include "pet_app.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy AI Passport / Codex 宠物启动");

    const esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", (int)wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是这个应用唯一的输出载体。失败就打清楚日志后退出, 不做串口降级 ——
    // 那会让入口文件复杂一倍, 也帮不上排线问题。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败, 无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    const esp_err_t err = pet_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "宠物应用启动失败: %s", esp_err_to_name(err));
    }
}
