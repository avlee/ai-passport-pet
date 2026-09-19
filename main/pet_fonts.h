// main/pet_fonts.h
// 应用字体入口。
//
// 基线只启用了 Montserrat, 它没有任何中文 glyph, 所以中文界面必须由
// assets/fonts/ 下生成的字库提供。生成与字符覆盖见 tools/gen_pet_fonts.py
// 和 tests/test_pet_font_coverage.py。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// 由 lv_font_conv 生成(assets/fonts/pet_font_16.c / pet_font_20.c)。
LV_FONT_DECLARE(pet_font_16);
LV_FONT_DECLARE(pet_font_20);

// 构造应用自己的可写字体描述符(只挂 fallback, 不改 Flash 里的字模)。
// 必须在 bsp_lvgl_init() 之后、创建任何 label 之前调用一次。
void pet_fonts_init(void);

// 正文 / 动态文本(Codex 输出), 16 px。
const lv_font_t *pet_font_body(void);

// 标题 / 状态标签, 20 px。
const lv_font_t *pet_font_title(void);

// 查询某个字体(含 fallback 链)是否真的含有该码点的字形。
// is_placeholder 为真的字形不算覆盖 —— 那正是「显示成方框」的情况。
bool pet_font_has_glyph(const lv_font_t *font, uint32_t codepoint);

// 检查一段 UTF-8 文本用 font 是否每个码点都有字形。缺失时把码点写进
// missing(若不为 NULL)并返回 false。用于启动自检与测试。
bool pet_font_covers_text(const lv_font_t *font, const char *utf8,
                          uint32_t *missing);
