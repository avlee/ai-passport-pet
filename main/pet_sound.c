// main/pet_sound.c —— 提示音播放器, 见 pet_sound.h。
#include "pet_sound.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 提示音数据。pet_sound_clips.c 由 tools/gen_pet_sounds.py 生成, 重生成后
// 这两个名字不变 —— 换片段内容不用动这里的代码, 在 CLIPS 表里换文件即可。
extern const int16_t pet_sound_taskdone_pcm[];
extern const size_t pet_sound_taskdone_len;

// 与生成脚本对拍的格式常量: 改了任一边都要两边一起改。
#define PET_SOUND_RATE      16000
#define PET_SOUND_BITS      16
#define PET_SOUND_CHANNELS  1

// 音量 0..100。首版 80 真机偏轻(2026-09-22 队长试听), 提到 90;
// 实际响度还取决于板上功放, 再不合适继续调这里。
#define PET_SOUND_VOLUME    90

// 每块喂给 I2S 的采样数。512 采样 = 1 KB 搬运缓冲(静态分配, 只有一个
// 播放任务用它), 与 demo_audio 的分块粒度一致。
#define PET_SOUND_CHUNK     512

static const char *TAG = "pet_sound";

static TaskHandle_t s_task;
static volatile bool s_enabled = true;
// 播放请求序号。播放中来了新请求 → 序号变化 → 当前这一遍作废, 重头再播:
// "任务完成"连响两遍比排队等两遍更像一个提醒。
static volatile uint32_t s_request;

void pet_sound_set_enabled(bool enabled)
{
    if (!enabled) s_request = 0;  // 静音期间来的请求一并作废, 恢复后不补放
    s_enabled = enabled;
}

void pet_sound_play(const char *clip)
{
    if (clip == NULL || clip[0] == '\0') return;
    if (strcmp(clip, "taskdone") != 0) {
        // 协议层不查白名单(见 pet_protocol.c), 白名单在这里: 片段名未知
        // 说明主机比固件新, 静默忽略而不是报错刷屏。
        ESP_LOGW(TAG, "未知提示音片段: %s", clip);
        return;
    }
    s_request++;
    if (s_task != NULL) xTaskNotify(s_task, 0, eNoAction);
}

// 播一遍 taskdone。返回时可能已经播完, 也可能中途被新请求顶掉 —— 由调用方
// 对比序号判断。
static void play_taskdone(void)
{
    // bsp_audio_init 幂等: 成功过的重复调用是廉价的。第一次播才真正
    // 打开 codec, 之前应用一直是纯显示, 不白付这份功耗。
    const esp_err_t init_err = bsp_audio_init();
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败: %s", esp_err_to_name(init_err));
        return;
    }
    if (bsp_audio_set_format(PET_SOUND_RATE, PET_SOUND_BITS,
                             PET_SOUND_CHANNELS) != ESP_OK) {
        ESP_LOGW(TAG, "设置采样格式失败");
        return;
    }
    bsp_audio_set_volume(PET_SOUND_VOLUME);

    // rodata 里的 PCM 不能直接当 I2S 写入的源 —— 从 flash cache 搬进
    // 内部 RAM 的一小块再写下去, 也顺便和 demo_audio 保持同一条路径。
    static int16_t chunk[PET_SOUND_CHUNK];
    const uint32_t request = s_request;

    ESP_LOGI(TAG, "播放 taskdone(%.2f 秒)",
             (double)pet_sound_taskdone_len / PET_SOUND_RATE);
    size_t played = 0;
    while (played < pet_sound_taskdone_len) {
        if (s_request != request || !s_enabled) return;  // 被顶掉/被静音
        size_t n = pet_sound_taskdone_len - played;
        if (n > PET_SOUND_CHUNK) n = PET_SOUND_CHUNK;
        memcpy(chunk, &pet_sound_taskdone_pcm[played], n * sizeof(int16_t));
        if (bsp_audio_write(chunk, n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "写入 PCM 失败, 本轮播放中止");
            return;
        }
        played += n;
    }
}

static void sound_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xTaskNotifyWait(0, 0, NULL, portMAX_DELAY) != pdTRUE) continue;
        // 序号还在变就继续播(重头): 播放期间来的请求不该被吞掉。
        while (s_enabled && s_request != 0) {
            const uint32_t request = s_request;
            play_taskdone();
            if (s_request == request) break;
        }
    }
}

esp_err_t pet_sound_start(void)
{
    if (s_task != NULL) return ESP_OK;
    if (xTaskCreate(sound_task, "pet_sound", 4096, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
