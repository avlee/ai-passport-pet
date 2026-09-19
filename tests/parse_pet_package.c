// tests/parse_pet_package.c
// 用设备侧的解析器读一份 .pet 文件, 把解出来的每个字段打到 stdout。
//
// 为什么要有这个挂具: 包的字节布局是**跨越两种语言**的契约 —— Python 侧按
// struct 格式写, C 侧按 pet_pkg_header_t 的偏移读。两边错开了既不会编译失败,
// 也不会运行报错, 只会让设备静默拒收或者画出乱码。所以让生成器真正写出来的字节
// 走一遍设备的解析器, 逐字段比对(tests/test_pet_package.py)。
//
// 用法:
//     parse_pet_package <package.pet>
// 退出码: 0 = 解析通过(不论 CRC 档位), 1 = 解析失败。
//
// 输出格式(第一行):
//     ok <id> <display> <frame_count> <state_count> <cell_w> <cell_h>
//        <stage_x> <stage_y> <stage_w> <stage_h> <blob_size> <total_size>
// 或者
//     error <原因>
// 之后每个状态一行:
//     state <name> <first> <count>
// 之后每帧一行:
//     frame <offset> <x> <y> <w> <h> <duration>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pet_pkg.h"

// 模拟 mmap: 设备映射的是整个 pets 分区(3.94 MB), 而包只占前面一段, 后面是擦成
// 0xFF 的空白。多留一点尾巴可以顺带验证"解析以包内声明长度为准, 多余字节被忽略"。
#define TRAILING_SLACK 512

static int report_error(pet_pkg_error_t error)
{
    printf("error %s\n", pet_pkg_error_name(error));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <package.pet>\n", argv[0]);
        return 2;
    }

    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        perror("fopen");
        return 2;
    }
    if (fseek(file, 0, SEEK_END) != 0) return 2;
    const long size = ftell(file);
    if (size <= 0) return 2;
    rewind(file);

    const size_t total = (size_t)size + TRAILING_SLACK;
    uint8_t *raw = malloc(total);
    if (raw == NULL) return 2;
    if (fread(raw, 1, (size_t)size, file) != (size_t)size) return 2;
    fclose(file);
    memset(raw + size, 0xFF, TRAILING_SLACK);

    pet_pkg_view_t view;
    // 整包 CRC 也一起校验: 这一条正是设备在 pet_slot_finish() 里做的那次。
    const pet_pkg_error_t error = pet_pkg_parse(raw, total, PET_PKG_CHECK_BLOB_CRC, &view);
    if (error != PET_PKG_OK) {
        const int status = report_error(error);
        free(raw);
        return status;
    }

    const pet_pkg_header_t *header = view.header;
    printf("ok %s %s %u %u %u %u %u %u %u %u %u %u\n",
           header->pet_id, header->display_name,
           (unsigned)header->frame_count, (unsigned)header->state_count,
           (unsigned)header->cell_w, (unsigned)header->cell_h,
           (unsigned)header->stage_x, (unsigned)header->stage_y,
           (unsigned)header->stage_w, (unsigned)header->stage_h,
           (unsigned)header->blob_size, (unsigned)header->total_size);

    for (unsigned i = 0; i < header->state_count; i++) {
        const pet_pkg_state_t *state = &view.states[i];
        printf("state %s %u %u\n", state->name, (unsigned)state->first,
               (unsigned)state->count);
    }
    for (unsigned i = 0; i < header->frame_count; i++) {
        const pet_pkg_frame_t *frame = &view.frames[i];
        printf("frame %u %d %d %d %d %u\n", (unsigned)frame->offset, (int)frame->x,
               (int)frame->y, (int)frame->w, (int)frame->h,
               (unsigned)frame->duration);
    }

    free(raw);
    return 0;
}
