// main/pet_pkg.c
#include "pet_pkg.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

// 头部字段的偏移写死在这里, 生成器(Python struct)按同样的表写字节。
// 用 _Static_assert 钉住, 免得哪天有人动了字段顺序而两边悄悄错开。
_Static_assert(offsetof(pet_pkg_header_t, magic) == 0, "header magic offset");
_Static_assert(offsetof(pet_pkg_header_t, header_size) == 6, "header_size offset");
_Static_assert(offsetof(pet_pkg_header_t, total_size) == 8, "total_size offset");
_Static_assert(offsetof(pet_pkg_header_t, frames_offset) == 24, "frames_offset");
_Static_assert(offsetof(pet_pkg_header_t, states_offset) == 28, "states_offset");
_Static_assert(offsetof(pet_pkg_header_t, blob_offset) == 32, "blob_offset");
_Static_assert(offsetof(pet_pkg_header_t, frame_count) == 36, "frame_count offset");
_Static_assert(offsetof(pet_pkg_header_t, cell_w) == 40, "cell_w offset");
_Static_assert(offsetof(pet_pkg_header_t, stage_x) == 44, "stage_x offset");
_Static_assert(offsetof(pet_pkg_header_t, stage_h) == 50, "stage_h offset");
_Static_assert(offsetof(pet_pkg_header_t, pet_id) == 52, "pet_id offset");
_Static_assert(offsetof(pet_pkg_header_t, display_name) == 84, "display_name");
_Static_assert(offsetof(pet_pkg_header_t, header_crc32) == 116, "header_crc32");
_Static_assert(sizeof(pet_pkg_header_t) == PET_PKG_HEADER_SIZE, "header size");
_Static_assert(sizeof(pet_pkg_frame_t) == 16, "frame record must be 16 bytes");
_Static_assert(sizeof(pet_pkg_state_t) == 20, "state record must be 20 bytes");

// 生成器按 pet_anim_t 的顺序输出动画行。顺序一旦错位, 动画会跳到别的动作上,
// 而且不会崩溃 —— 所以必须显式校验名字。
static const char *const EXPECTED_STATE_NAMES[PET_ANIM_COUNT] = {
    "idle",   "running_right", "running_left", "waving", "jumping",
    "failed", "waiting",       "running",      "review",
};

// 单帧动画看不出动作: 每行至少两帧。
#define PET_PKG_MIN_FRAMES_PER_STATE 2

#define HEADER_CRC_OFFSET offsetof(pet_pkg_header_t, header_crc32)

// ---------------------------------------------------------------- CRC32 -----
// 查表法。表按需生成一次(1 KB), 比逐位快一个数量级 —— 换宠物时要校验 3 MB。
static uint32_t s_crc_table[256];
static bool     s_crc_ready;

static void crc32_build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; bit++) {
            value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        }
        s_crc_table[i] = value;
    }
    s_crc_ready = true;
}

uint32_t pet_pkg_crc32_begin(void)
{
    if (!s_crc_ready) crc32_build_table();
    return 0xFFFFFFFFu;
}

uint32_t pet_pkg_crc32_step(uint32_t crc, uint8_t byte)
{
    return s_crc_table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
}

uint32_t pet_pkg_crc32_finish(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint32_t pet_pkg_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = pet_pkg_crc32_begin();
    for (size_t i = 0; i < size; i++) crc = pet_pkg_crc32_step(crc, bytes[i]);
    return pet_pkg_crc32_finish(crc);
}

// 头部 CRC 的口径: pet_pkg_header_t 的原始字节, header_crc32 字段按 0 参与。
// 这样校验的一方不需要先把头部清零再算。
static uint32_t header_crc32(const uint8_t *data)
{
    uint32_t crc = pet_pkg_crc32_begin();
    for (size_t i = 0; i < PET_PKG_HEADER_SIZE; i++) {
        const bool in_crc_field =
            i >= HEADER_CRC_OFFSET && i < HEADER_CRC_OFFSET + sizeof(uint32_t);
        crc = pet_pkg_crc32_step(crc, in_crc_field ? 0 : data[i]);
    }
    return pet_pkg_crc32_finish(crc);
}

// --------------------------------------------------------------- 小工具 -----
// 定长字符数组: 必须在长度内自成一个字符串。
static bool fixed_string_ok(const char *field, size_t capacity)
{
    if (field[0] == '\0') return false;
    return memchr(field, '\0', capacity) != NULL;
}

static bool range_within(uint32_t offset, uint32_t length, uint32_t limit)
{
    if (offset > limit) return false;
    return length <= limit - offset;
}

const char *pet_pkg_error_name(pet_pkg_error_t error)
{
    switch (error) {
    case PET_PKG_OK:                 return "ok";
    case PET_PKG_ERR_SHORT:          return "too short";
    case PET_PKG_ERR_MAGIC:          return "bad magic";
    case PET_PKG_ERR_VERSION:        return "unsupported version";
    case PET_PKG_ERR_HEADER_SIZE:    return "bad header size";
    case PET_PKG_ERR_HEADER_CRC:     return "header crc";
    case PET_PKG_ERR_TOTAL_SIZE:     return "declared size too large";
    case PET_PKG_ERR_ID:             return "bad pet id";
    case PET_PKG_ERR_FRAME_COUNT:    return "bad frame count";
    case PET_PKG_ERR_STATE_COUNT:    return "state count mismatch";
    case PET_PKG_ERR_STATE_NAME:     return "state name mismatch";
    case PET_PKG_ERR_STATE_LAYOUT:   return "state layout";
    case PET_PKG_ERR_FRAME_GEOMETRY: return "frame geometry";
    case PET_PKG_ERR_FRAME_RANGE:    return "frame outside blob";
    case PET_PKG_ERR_FRAME_ORDER:    return "frame order";
    case PET_PKG_ERR_TABLES_CRC:     return "table crc";
    case PET_PKG_ERR_BLOB_CRC:       return "blob crc";
    case PET_PKG_ERR_STAGE:          return "stage box";
    default:                         return "unknown";
    }
}

// ------------------------------------------------------------------ 解析 ----
// 头部自检。刻意不比对 buffer 长度: 调用方可能只读了 120 字节就进来问"这包多长"。
static pet_pkg_error_t check_header(const uint8_t *data, size_t size,
                                    const pet_pkg_header_t **out)
{
    if (data == NULL || out == NULL) return PET_PKG_ERR_SHORT;
    if (size < PET_PKG_HEADER_SIZE) return PET_PKG_ERR_SHORT;

    const pet_pkg_header_t *header = (const pet_pkg_header_t *)data;

    if (header->magic != PET_PKG_MAGIC) return PET_PKG_ERR_MAGIC;
    if (header->version != PET_PKG_VERSION) return PET_PKG_ERR_VERSION;
    if (header->header_size != PET_PKG_HEADER_SIZE) return PET_PKG_ERR_HEADER_SIZE;
    if (header_crc32(data) != header->header_crc32) return PET_PKG_ERR_HEADER_CRC;

    if (header->total_size < PET_PKG_HEADER_SIZE) return PET_PKG_ERR_TOTAL_SIZE;

    if (!fixed_string_ok(header->pet_id, PET_PKG_ID_MAX)) return PET_PKG_ERR_ID;
    if (!fixed_string_ok(header->display_name, PET_PKG_NAME_MAX)) {
        return PET_PKG_ERR_ID;
    }

    *out = header;
    return PET_PKG_OK;
}

pet_pkg_error_t pet_pkg_read_header(const uint8_t *data, size_t size,
                                    pet_pkg_header_t *out)
{
    if (out == NULL) return PET_PKG_ERR_SHORT;

    const pet_pkg_header_t *header = NULL;
    const pet_pkg_error_t err = check_header(data, size, &header);
    if (err != PET_PKG_OK) return err;

    *out = *header;
    return PET_PKG_OK;
}

pet_pkg_error_t pet_pkg_parse(const uint8_t *data, size_t size,
                              pet_pkg_check_t check, pet_pkg_view_t *out)
{
    if (out == NULL) return PET_PKG_ERR_SHORT;

    const pet_pkg_header_t *header = NULL;
    pet_pkg_error_t err = check_header(data, size, &header);
    if (err != PET_PKG_OK) return err;

    // size 允许大于包长(设备上 mmap 的是整块分区), 但包里声明的一定要装得下。
    if (header->total_size > size) return PET_PKG_ERR_TOTAL_SIZE;

    if (header->state_count != PET_ANIM_COUNT) return PET_PKG_ERR_STATE_COUNT;
    if (header->frame_count == 0 || header->frame_count > PET_PKG_FRAME_MAX) {
        return PET_PKG_ERR_FRAME_COUNT;
    }
    if (header->cell_w != PET_PKG_CELL_W || header->cell_h != PET_PKG_CELL_H) {
        return PET_PKG_ERR_FRAME_GEOMETRY;
    }

    const uint32_t frames_bytes =
        (uint32_t)header->frame_count * (uint32_t)sizeof(pet_pkg_frame_t);
    const uint32_t states_bytes =
        (uint32_t)header->state_count * (uint32_t)sizeof(pet_pkg_state_t);

    if ((header->frames_offset % 4) != 0 || (header->states_offset % 4) != 0 ||
        (header->blob_offset % 4) != 0) {
        return PET_PKG_ERR_FRAME_ORDER;
    }
    // 表区必须是"帧表紧跟状态表"这一整段: 校验和按整段算, 排布也不能随意。
    if (header->states_offset != header->frames_offset + frames_bytes) {
        return PET_PKG_ERR_TOTAL_SIZE;
    }
    if (!range_within(header->frames_offset, frames_bytes, header->total_size) ||
        !range_within(header->states_offset, states_bytes, header->total_size) ||
        !range_within(header->blob_offset, header->blob_size, header->total_size)) {
        return PET_PKG_ERR_TOTAL_SIZE;
    }
    if (header->blob_offset < header->states_offset + states_bytes) {
        return PET_PKG_ERR_TOTAL_SIZE;
    }

    const uint8_t *frames_raw = data + header->frames_offset;
    // 表区 = [frames_offset, blob_offset): 帧表 + 状态表 + 对齐填充。生成器按
    // 同样的范围算这条校验和, 所以填充字节也必须确定(全 0)。
    if (pet_pkg_crc32(frames_raw, header->blob_offset - header->frames_offset) !=
        header->tables_crc32) {
        return PET_PKG_ERR_TABLES_CRC;
    }

    // 整块像素的校验是可选的: 3 MB 要走一遍表, 开机时没必要 ——下载落盘那一次
    // 已经逐字节算过了。
    if (check == PET_PKG_CHECK_BLOB_CRC &&
        pet_pkg_crc32(data + header->blob_offset, header->blob_size) !=
            header->blob_crc32) {
        return PET_PKG_ERR_BLOB_CRC;
    }

    const pet_pkg_state_t *states =
        (const pet_pkg_state_t *)(data + header->states_offset);
    const pet_pkg_frame_t *frames = (const pet_pkg_frame_t *)frames_raw;
    const uint8_t *blob = data + header->blob_offset;

    uint32_t next_expected_first = 0;
    int32_t box_x0 = INT32_MAX, box_y0 = INT32_MAX;
    int32_t box_x1 = -1, box_y1 = -1;
    uint32_t previous_end = 0;

    for (int s = 0; s < PET_ANIM_COUNT; s++) {
        const pet_pkg_state_t *state = &states[s];

        if (!fixed_string_ok(state->name, PET_PKG_STATE_NAME_MAX)) {
            return PET_PKG_ERR_STATE_NAME;
        }
        if (strcmp(state->name, EXPECTED_STATE_NAMES[s]) != 0) {
            return PET_PKG_ERR_STATE_NAME;
        }
        if (state->count < PET_PKG_MIN_FRAMES_PER_STATE) {
            return PET_PKG_ERR_STATE_LAYOUT;
        }
        if (state->first != next_expected_first) return PET_PKG_ERR_STATE_LAYOUT;

        for (uint16_t i = 0; i < state->count; i++) {
            const uint32_t index = (uint32_t)state->first + i;
            if (index >= header->frame_count) return PET_PKG_ERR_FRAME_COUNT;

            const pet_pkg_frame_t *frame = &frames[index];
            if (frame->w <= 0 || frame->h <= 0) return PET_PKG_ERR_FRAME_GEOMETRY;
            if (frame->x < 0 || frame->y < 0 ||
                frame->x + frame->w > PET_PKG_CELL_W ||
                frame->y + frame->h > PET_PKG_CELL_H) {
                return PET_PKG_ERR_FRAME_GEOMETRY;
            }
            if (frame->duration == 0) return PET_PKG_ERR_FRAME_GEOMETRY;

            const uint32_t bytes = (uint32_t)frame->w * (uint32_t)frame->h *
                                   (uint32_t)PET_PKG_BYTES_PER_PIXEL;
            if (!range_within(frame->offset, bytes, header->blob_size)) {
                return PET_PKG_ERR_FRAME_RANGE;
            }
            // 帧数据必须严格递增、不重叠, 且 4 字节对齐 —— 渲染时按 uint16 读
            // RGB565 半字, 半字错位在 C3 上会直接触发异常。
            if (frame->offset < previous_end || (frame->offset % 4) != 0) {
                return PET_PKG_ERR_FRAME_ORDER;
            }
            previous_end = frame->offset + bytes;

            if (frame->x < box_x0) box_x0 = frame->x;
            if (frame->y < box_y0) box_y0 = frame->y;
            if (frame->x + frame->w > box_x1) box_x1 = frame->x + frame->w;
            if (frame->y + frame->h > box_y1) box_y1 = frame->y + frame->h;
        }

        next_expected_first += state->count;
    }

    if (next_expected_first != header->frame_count) {
        return PET_PKG_ERR_FRAME_COUNT;
    }

    // 舞台外框必须正好是所有帧裁剪区的并集: UI 靠它把每帧摆到正确位置,
    // 对不上就会整体偏移, 而且偏得不多时很难看出来。
    if (box_x1 <= box_x0 || box_y1 <= box_y0) return PET_PKG_ERR_STAGE;
    if (header->stage_x != (uint16_t)box_x0 || header->stage_y != (uint16_t)box_y0 ||
        header->stage_w != (uint16_t)(box_x1 - box_x0) ||
        header->stage_h != (uint16_t)(box_y1 - box_y0)) {
        return PET_PKG_ERR_STAGE;
    }

    out->header = header;
    out->states = states;
    out->frames = frames;
    out->blob = blob;
    return PET_PKG_OK;
}
