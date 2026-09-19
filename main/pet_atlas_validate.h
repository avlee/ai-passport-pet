// main/pet_atlas_validate.h
// 生成图集的纯逻辑自检: 不依赖 ESP-IDF / LVGL, 因此可以在主机测试里跑,
// 也可以在真机启动时跑一遍。换宠物图集或改生成脚本后, 这里会第一时间发现
// 「行顺序错位 / 帧越界 / 帧重叠」这类会直接毁掉动画的静默错误。
#pragma once

#include <stdint.h>

// 校验通过返回 0; 否则返回一个稳定的非零错误码(见实现里的注释)。
// blob_size 是嵌入的帧数据总字节数。
uint32_t pet_atlas_validate(uint32_t blob_size);
