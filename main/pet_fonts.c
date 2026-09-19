// main/pet_fonts.c
#include "pet_fonts.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "pet_fonts";

// 可写副本。lv_font_t 是只读的 Flash 常量, 直接改它属于「抹掉 const 改常量」,
// 这里按官方建议做一次浅拷贝: 字模数据仍在 Flash, 只多挂一个 fallback。
static lv_font_t s_body;
static lv_font_t s_title;
static bool s_ready;

void pet_fonts_init(void)
{
    if (s_ready) return;

    s_body = pet_font_16;
    // fallback 只用来补中文子集之外的符号类字形(例如 LV_SYMBOL_*)。
    // 它补不了缺失的汉字 —— 那属于子集范围问题, 见生成脚本。
    s_body.fallback = &lv_font_montserrat_20;

    s_title = pet_font_20;
    s_title.fallback = &lv_font_montserrat_20;

    s_ready = true;
    ESP_LOGI(TAG, "字体就绪: body=pet_font_16(行高 %d) title=pet_font_20(行高 %d)",
             s_body.line_height, s_title.line_height);
}

const lv_font_t *pet_font_body(void)
{
    return s_ready ? &s_body : (const lv_font_t *)&pet_font_16;
}

const lv_font_t *pet_font_title(void)
{
    return s_ready ? &s_title : (const lv_font_t *)&pet_font_20;
}

bool pet_font_has_glyph(const lv_font_t *font, uint32_t codepoint)
{
    if (font == NULL) return false;
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    if (!lv_font_get_glyph_dsc(font, &glyph, codepoint, 0)) return false;
    // 占位字形仍然算「没有覆盖」, 否则方框会被当成正常渲染。
    return !glyph.is_placeholder;
}

// 从 UTF-8 里取出下一个码点。非法序列按单字节跳过, 保证循环一定前进。
static uint32_t next_codepoint(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    uint32_t codepoint = 0;
    size_t length = 1;

    if (p[0] < 0x80) {
        codepoint = p[0];
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        length = 2;
    } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x0F) << 12) |
                    ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
        length = 3;
    } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x07) << 18) |
                    ((uint32_t)(p[1] & 0x3F) << 12) |
                    ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        length = 4;
    } else {
        codepoint = p[0];
    }

    *cursor += length;
    return codepoint;
}

bool pet_font_covers_text(const lv_font_t *font, const char *utf8,
                          uint32_t *missing)
{
    if (font == NULL || utf8 == NULL) return false;
    const char *cursor = utf8;
    while (*cursor != '\0') {
        const uint32_t codepoint = next_codepoint(&cursor);
        // 控制字符不参与覆盖检查。
        if (codepoint < 0x20 || codepoint == 0x7F) continue;
        if (!pet_font_has_glyph(font, codepoint)) {
            if (missing != NULL) *missing = codepoint;
            return false;
        }
    }
    return true;
}
