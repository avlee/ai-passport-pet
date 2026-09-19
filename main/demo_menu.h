// main/demo_menu.h
// BSP 参考示例的演示菜单。
//
// 这段逻辑原本就是 main.c 的全部内容。新的 app_main 开机直接进 Codex 宠物界面,
// 演示菜单改由宠物界面的信息面板进入(长按上键), 这样既保住了仓库作为
// 「BSP 驱动参考」的价值, 又不会让菜单挡住宠物。
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// 创建菜单并处理按键, 直到用户在主菜单长按「确定」。
// 本函数会阻塞当前任务, 并自行从 input_queue 取按键事件
// (队列的创建与消费方在 pet_app.c)。
void demo_menu_run(QueueHandle_t input_queue);
