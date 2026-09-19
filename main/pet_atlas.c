// main/pet_atlas.c
#include "pet_atlas.h"

#include <string.h>

#include "esp_log.h"
#include "pet_layout.h"
#include "pet_slot.h"

static const char *TAG = "pet_atlas";

// 帧描述符。索引与包里的帧表一一对应(state->first + index)。
// 按上限静态分配: 帧数现在是运行时才知道的, 但描述符数组必须在编译期定下来。
static lv_image_dsc_t s_images[PET_PKG_FRAME_MAX];
static bool           s_ready;

static const pet_pkg_view_t *view(void)
{
    return s_ready ? pet_slot_view() : NULL;
}

static const pet_pkg_state_t *state_of(pet_anim_t anim)
{
    const pet_pkg_view_t *pkg = view();
    if (pkg == NULL) return NULL;
    if (anim < 0 || anim >= PET_ANIM_COUNT) return NULL;
    return &pkg->states[anim];
}

bool pet_atlas_ready(void)
{
    return s_ready;
}

const char *pet_atlas_pet_id(void)
{
    return s_ready ? pet_slot_pet_id() : PET_SLOT_NO_PET;
}

const char *pet_atlas_display_name(void)
{
    const pet_pkg_view_t *pkg = view();
    return pkg != NULL ? pkg->header->display_name : "";
}

uint16_t pet_atlas_frame_count(pet_anim_t anim)
{
    const pet_pkg_state_t *state = state_of(anim);
    return state != NULL ? state->count : 0;
}

const pet_pkg_frame_t *pet_atlas_frame(pet_anim_t anim, uint16_t index)
{
    const pet_pkg_state_t *state = state_of(anim);
    if (state == NULL || index >= state->count) return NULL;
    return &view()->frames[state->first + index];
}

const lv_image_dsc_t *pet_atlas_image(pet_anim_t anim, uint16_t index)
{
    const pet_pkg_state_t *state = state_of(anim);
    if (state == NULL || index >= state->count) return NULL;
    return &s_images[state->first + index];
}

uint16_t pet_atlas_stage_x(void)
{
    const pet_pkg_view_t *pkg = view();
    return pkg != NULL ? pkg->header->stage_x : 0;
}

uint16_t pet_atlas_stage_y(void)
{
    const pet_pkg_view_t *pkg = view();
    return pkg != NULL ? pkg->header->stage_y : 0;
}

uint16_t pet_atlas_stage_width(void)
{
    const pet_pkg_view_t *pkg = view();
    return pkg != NULL ? pkg->header->stage_w : 0;
}

uint16_t pet_atlas_stage_height(void)
{
    const pet_pkg_view_t *pkg = view();
    return pkg != NULL ? pkg->header->stage_h : 0;
}

void pet_atlas_unload(void)
{
    s_ready = false;
    memset(s_images, 0, sizeof(s_images));
    pet_slot_unmount();
}

bool pet_atlas_load(void)
{
    pet_atlas_unload();

    const esp_err_t err = pet_slot_mount();
    if (err != ESP_OK) {
        // 槽是空的或者包坏了。这不是致命错误: 设备照常跑, 界面显示"还没有宠物",
        // 菜单栏看到 hello 里的 pet=none 会自己把默认那只推下来。
        ESP_LOGW(TAG, "没有可用的宠物(%s)", pet_slot_last_error());
        return false;
    }

    const pet_pkg_view_t *pkg = pet_slot_view();
    const pet_pkg_header_t *header = pkg->header;

    // 包的内部自洽由 pet_pkg_parse() 保证, 但它管不到"画不画得下" —— 那是版式的
    // 事(pet_layout.c)。所以在这里补上: 舞台过宽会被 30 px 圆角切到, 过高会顶进
    // 顶部状态行。宁可拒绝这只宠物(界面提示"还没有宠物"), 也不画出一个错位的界面。
    if (!pet_layout_stage_supported((int)header->stage_w, (int)header->stage_h)) {
        ESP_LOGE(TAG, "宠物 %s 的舞台 %ux%u 放不下这块屏, 拒绝装载",
                 header->pet_id, header->stage_w, header->stage_h);
        pet_slot_unmount();
        return false;
    }

    for (uint16_t i = 0; i < header->frame_count; i++) {
        const pet_pkg_frame_t *frame = &pkg->frames[i];
        lv_image_dsc_t *image = &s_images[i];

        memset(image, 0, sizeof(*image));
        image->header.magic = LV_IMAGE_HEADER_MAGIC;
        // RGB565A8: 颜色平面用 w*2 作为 stride, alpha 平面紧随其后。
        image->header.cf = LV_COLOR_FORMAT_RGB565A8;
        image->header.w = (uint16_t)frame->w;
        image->header.h = (uint16_t)frame->h;
        image->header.stride = (uint16_t)(frame->w * 2);
        image->data_size = (uint32_t)frame->w * (uint32_t)frame->h *
                           PET_PKG_BYTES_PER_PIXEL;
        // 关键: 直接指进 mmap 出来的只读 flash, 不复制像素。
        image->data = pkg->blob + frame->offset;
    }

    s_ready = true;
    ESP_LOGI(TAG, "图集就绪: %s (%s), %u 帧, 绘制外框 %ux%u @ (%u,%u)",
             header->pet_id, header->display_name, (unsigned)header->frame_count,
             header->stage_w, header->stage_h, header->stage_x, header->stage_y);
    return true;
}
