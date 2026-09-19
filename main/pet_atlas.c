// main/pet_atlas.c
#include "pet_atlas.h"

#include <string.h>

#include "esp_log.h"
#include "pet_atlas_validate.h"

static const char *TAG = "pet_atlas";

// EMBED_FILES 把 main/assets/pet_sophie_portrait.bin 链接成一对符号。
extern const uint8_t pet_atlas_blob_start[] asm("_binary_pet_sophie_portrait_bin_start");
extern const uint8_t pet_atlas_blob_end[] asm("_binary_pet_sophie_portrait_bin_end");

// 生成的动画行顺序必须与 pet_anim_t 一致, 否则帧会错位到别的动作上。
// pet_atlas_validate() 会在启动时做更完整的一致性检查。
_Static_assert(PET_ATLAS_STATE_COUNT == PET_ANIM_COUNT,
               "generated atlas rows must match pet_anim_t");

static lv_image_dsc_t s_images[PET_ATLAS_FRAME_COUNT];
static bool s_ready;

static const pet_atlas_state_t *state_of(pet_anim_t anim)
{
    if (anim < 0 || anim >= PET_ATLAS_STATE_COUNT) return NULL;
    return &PET_ATLAS_STATES[anim];
}

uint16_t pet_atlas_frame_count(pet_anim_t anim)
{
    const pet_atlas_state_t *state = state_of(anim);
    return state ? state->count : 0;
}

const pet_atlas_frame_t *pet_atlas_frame(pet_anim_t anim, uint16_t index)
{
    const pet_atlas_state_t *state = state_of(anim);
    if (state == NULL || index >= state->count) return NULL;
    return &state->frames[index];
}

const lv_image_dsc_t *pet_atlas_image(pet_anim_t anim, uint16_t index)
{
    const pet_atlas_state_t *state = state_of(anim);
    if (state == NULL || index >= state->count) return NULL;
    return &s_images[state->first + index];
}

uint16_t pet_atlas_stage_width(void)
{
    return PET_ATLAS_STAGE_W;
}

uint16_t pet_atlas_stage_height(void)
{
    return PET_ATLAS_STAGE_H;
}

void pet_atlas_init(void)
{
    if (s_ready) return;

    const size_t blob_size = (size_t)(pet_atlas_blob_end - pet_atlas_blob_start);

    const uint32_t error = pet_atlas_validate((uint32_t)blob_size);
    if (error != 0) {
        // 帧表与 blob 不一致时继续跑只会显示错位的画面, 所以明确报错。
        ESP_LOGE(TAG, "图集自检失败: 错误码 %u (blob=%u 字节)", (unsigned)error,
                 (unsigned)blob_size);
        return;
    }

    for (int state_index = 0; state_index < PET_ATLAS_STATE_COUNT; state_index++) {
        const pet_atlas_state_t *state = &PET_ATLAS_STATES[state_index];
        for (uint16_t i = 0; i < state->count; i++) {
            const pet_atlas_frame_t *frame = &state->frames[i];
            lv_image_dsc_t *image = &s_images[state->first + i];

            memset(image, 0, sizeof(*image));
            image->header.magic = LV_IMAGE_HEADER_MAGIC;
            // RGB565A8: 颜色平面用 w*2 作为 stride, alpha 平面紧随其后。
            image->header.cf = LV_COLOR_FORMAT_RGB565A8;
            image->header.w = (uint16_t)frame->w;
            image->header.h = (uint16_t)frame->h;
            image->header.stride = (uint16_t)(frame->w * 2);
            image->data_size = (uint32_t)frame->w * (uint32_t)frame->h *
                               PET_ATLAS_BYTES_PER_PIXEL;
            image->data = pet_atlas_blob_start + frame->offset;
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "图集就绪: %d 帧, blob=%u 字节, 绘制外框 %dx%d",
             PET_ATLAS_FRAME_COUNT, (unsigned)blob_size, PET_ATLAS_STAGE_W,
             PET_ATLAS_STAGE_H);
}
