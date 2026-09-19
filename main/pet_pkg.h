// main/pet_pkg.h
// 宠物包(.pet)的解析与自检。
//
// 宠物资产不再编进 app 分区。它躺在独立的 `pets` 数据分区里(mmap 只读),
// 由本模块解释; 换宠物 = 往那个分区写一份新的包, 不需要重新编译固件。
//
// 本文件刻意不依赖 ESP-IDF 与 LVGL: 包格式、帧表布局、几何约束都是最容易
// "写错了还能跑"的地方(表现是动画错位或花屏), 必须能在主机上直接钉死。
// 见 tests/test_pet_pkg.c。
//
// 包布局(小端, 全部 4 字节对齐):
//
//   +0               pet_pkg_header_t
//   +frames_offset   pet_pkg_frame_t[frame_count]
//   +states_offset   pet_pkg_state_t[state_count]
//   +blob_offset     逐帧 RGB565A8 像素数据
//
// 三块内容分别由 header_crc32 / tables_crc32 / blob_crc32 保护。表区特意排成
// 连续的一整段, 这样一条 CRC 就能覆盖帧表和状态表 —— 分开保护的话状态表就没有
// 任何校验, 而它的错位恰好是最难从画面上看出来的。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pet_state.h"

// "PET1" —— 小端读出来的值。分区被擦过时全是 0xFF, 一眼就能区分。
#define PET_PKG_MAGIC   0x31544550u
#define PET_PKG_VERSION 1

#define PET_PKG_ID_MAX         32
#define PET_PKG_NAME_MAX       32
#define PET_PKG_STATE_NAME_MAX 16

// 头部是定长结构, 生成器按同样的字节布局写。
#define PET_PKG_HEADER_SIZE 120

// 帧描述符数组要在设备上静态分配, 所以帧数必须有编译期上限。
// 现有宠物 57 帧, 留一倍余量。
#define PET_PKG_FRAME_MAX 128

// 每像素 3 字节: RGB565 颜色平面 + 8 位 alpha 平面。
#define PET_PKG_BYTES_PER_PIXEL 3

// 图集单元格。v2 素材固定 192x208, 包里的每一帧都是它的一个紧裁子矩形。
#define PET_PKG_CELL_W 192
#define PET_PKG_CELL_H 208

typedef struct {
    uint32_t offset;    // 该帧像素在 blob 内的字节偏移
    int16_t  x;         // 裁剪原点(相对 192x208 单元格)
    int16_t  y;
    int16_t  w;
    int16_t  h;
    uint16_t duration;  // 该帧的毫秒时长
    uint16_t reserved;  // 显式补齐到 16 字节, 免得依赖编译器隐式填充
} pet_pkg_frame_t;

typedef struct {
    char     name[PET_PKG_STATE_NAME_MAX];
    uint16_t first;
    uint16_t count;
} pet_pkg_state_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    uint32_t total_size;    // 整个包的字节数
    uint32_t blob_size;     // 像素数据字节数
    uint32_t blob_crc32;    // 像素数据 CRC32
    uint32_t tables_crc32;  // 表区(frames_offset 到 blob_offset)的 CRC32

    uint32_t frames_offset;
    uint32_t states_offset;
    uint32_t blob_offset;

    uint16_t frame_count;
    uint16_t state_count;

    uint16_t cell_w;        // 恒为 PET_PKG_CELL_W, 留字段是为了将来能换网格
    uint16_t cell_h;

    // 所有帧裁剪区的并集 —— 也就是屏幕上那块固定大小的绘制外框, 以及
    // 每帧裁剪原点要减掉的那个基准点。
    uint16_t stage_x;
    uint16_t stage_y;
    uint16_t stage_w;
    uint16_t stage_h;

    char     pet_id[PET_PKG_ID_MAX];
    char     display_name[PET_PKG_NAME_MAX];

    uint32_t header_crc32;  // 本字段按 0 计算
} pet_pkg_header_t;

// 解析结果。指针全部指向传入的那块内存(设备上是 mmap 出来的 flash), 不复制。
typedef struct {
    const pet_pkg_header_t *header;
    const pet_pkg_state_t  *states;
    const pet_pkg_frame_t  *frames;
    const uint8_t          *blob;
} pet_pkg_view_t;

typedef enum {
    PET_PKG_OK = 0,
    PET_PKG_ERR_SHORT,          // 连头部都放不下
    PET_PKG_ERR_MAGIC,          // 不是宠物包(分区空或被擦过)
    PET_PKG_ERR_VERSION,
    PET_PKG_ERR_HEADER_SIZE,
    PET_PKG_ERR_HEADER_CRC,
    PET_PKG_ERR_TOTAL_SIZE,     // 包里声明的长度超出实际数据
    PET_PKG_ERR_ID,             // pet_id 为空或没有 NUL 结尾
    PET_PKG_ERR_FRAME_COUNT,    // 帧数超上限或与状态表不一致
    PET_PKG_ERR_STATE_COUNT,    // 动画行数与 pet_anim_t 不一致
    PET_PKG_ERR_STATE_NAME,     // 行名/顺序与 pet_anim_t 不符
    PET_PKG_ERR_STATE_LAYOUT,   // first/count 不连续, 或某行帧数太少
    PET_PKG_ERR_FRAME_GEOMETRY, // 宽高非法, 或裁剪区超出单元格
    PET_PKG_ERR_FRAME_RANGE,    // 帧数据越出 blob
    PET_PKG_ERR_FRAME_ORDER,    // 帧数据重叠、未递增或未 4 字节对齐
    PET_PKG_ERR_TABLES_CRC,
    PET_PKG_ERR_BLOB_CRC,
    PET_PKG_ERR_STAGE,          // 舞台并集与逐帧并集对不上
} pet_pkg_error_t;

// 校验深度。逐字节算 CRC 要走完整块 blob(3 MB 约 0.4 s), 所以分成两档:
// 开机只需要结构正确, 完整校验放在下载落盘那一次(那里本来就在逐字节收)。
typedef enum {
    PET_PKG_CHECK_STRUCTURE = 0,  // 头部 + 帧表 CRC + 几何
    PET_PKG_CHECK_BLOB_CRC = 1,   // 另外逐字节校验像素数据
} pet_pkg_check_t;

// 解析并自检。成功返回 PET_PKG_OK 且填好 *out; 失败返回具体错误码, *out 不变。
// size 可以大于包子声明的长度(设备上 mmap 的是整块分区), 以包内声明为准。
pet_pkg_error_t pet_pkg_parse(const uint8_t *data, size_t size,
                              pet_pkg_check_t check, pet_pkg_view_t *out);

// 只读头部。设备先要知道包有多长才能决定映射多少页 —— 把 3.94 MB 的分区整块
// 映射进去会白白吃掉 MMU 页, 而那些页和 app 的 rodata 是共用同一份的。
// 只校验头部 CRC 与身份字段, 不检查长度是否落在 buffer 内。
pet_pkg_error_t pet_pkg_read_header(const uint8_t *data, size_t size,
                                    pet_pkg_header_t *out);

// 错误码的稳定名字, 用于日志与协议回执。
const char *pet_pkg_error_name(pet_pkg_error_t error);

// CRC32(IEEE 802.3, 反射, 初值 0xFFFFFFFF, 末尾取反)—— 与 Python 的
// zlib.crc32 / binascii.crc32 完全一致, 所以生成器算出来的值设备能直接对。
uint32_t pet_pkg_crc32(const void *data, size_t size);

// 增量版本。设备是一边从链路收字节一边写 flash 的, 没有"完整的一份"可以先攒起来
// 再算, 所以校验必须能边收边算。
uint32_t pet_pkg_crc32_begin(void);
uint32_t pet_pkg_crc32_step(uint32_t crc, uint8_t byte);
uint32_t pet_pkg_crc32_finish(uint32_t crc);
