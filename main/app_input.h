// main/app_input.h
// 按键事件从回调搬到工作任务的公共类型。
//
// 按键回调运行在共享的 esp_timer 任务上(bsp_button.h 要求回调只能入队), 所以
// 需要一个队列把事件搬走。宠物界面和演示菜单会互相交接这个队列, 因此事件类型
// 放在公共头文件里, 避免两边各写一份结构体靠内存布局巧合对齐。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    bsp_btn_t    btn;
    bsp_btn_ev_t event;
} app_input_t;

#define APP_INPUT_QUEUE_DEPTH 8

static inline bool app_input_receive(QueueHandle_t queue, app_input_t *out,
                                     uint32_t timeout_ms)
{
    if (queue == NULL || out == NULL) return false;
    return xQueueReceive(queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
