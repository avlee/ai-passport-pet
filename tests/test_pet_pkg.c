// tests/test_pet_pkg.c
// 宠物包解析器的单元测试(主机)。
//
// 这里手工拼一个"最小但合法"的包, 再逐条把它改坏。包格式的错误几乎全都表现成
// "帧错位/花屏"而不是崩溃, 所以每一条约束都要有一发反例钉住。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pet_pkg.h"

#define STATES  PET_ANIM_COUNT
#define PER_ROW 2
#define FRAMES  (STATES * PER_ROW)

#define FRAME_W 4
#define FRAME_H 4
#define FRAME_BYTES (FRAME_W * FRAME_H * PET_PKG_BYTES_PER_PIXEL)

#define STAGE_X0 10
#define STAGE_Y0 20

#define FRAMES_AT   PET_PKG_HEADER_SIZE
#define STATES_AT   (FRAMES_AT + FRAMES * (int)sizeof(pet_pkg_frame_t))
#define BLOB_AT     (STATES_AT + STATES * (int)sizeof(pet_pkg_state_t))
#define TOTAL_SIZE  (BLOB_AT + FRAMES * FRAME_BYTES)

static const char *const STATE_NAMES[STATES] = {
    "idle",   "running_right", "running_left", "waving", "jumping",
    "failed", "waiting",       "running",      "review",
};

// 留一截尾巴: 设备上是把整块分区 mmap 进来的, 缓冲区比包本身大是常态。
#define SLACK 512
static uint8_t  g_pkg[TOTAL_SIZE + SLACK];
static uint16_t g_frame_offset[FRAMES];

static int g_failures;

static void check(int condition, const char *what)
{
    if (condition) return;
    printf("FAIL: %s\n", what);
    g_failures++;
}

// 把包按"完全合法"填一遍。返回后用 build_tables() 收尾。
static void build_package(void)
{
    memset(g_pkg, 0, sizeof(g_pkg));
    pet_pkg_header_t *header = (pet_pkg_header_t *)g_pkg;

    header->magic = PET_PKG_MAGIC;
    header->version = PET_PKG_VERSION;
    header->header_size = PET_PKG_HEADER_SIZE;
    header->total_size = TOTAL_SIZE;
    header->blob_size = FRAMES * FRAME_BYTES;
    header->frames_offset = FRAMES_AT;
    header->states_offset = STATES_AT;
    header->blob_offset = BLOB_AT;
    header->frame_count = FRAMES;
    header->state_count = STATES;
    header->cell_w = PET_PKG_CELL_W;
    header->cell_h = PET_PKG_CELL_H;
    header->stage_x = STAGE_X0;
    header->stage_y = STAGE_Y0;
    header->stage_w = FRAME_W + (PER_ROW - 1) * 2;
    header->stage_h = FRAME_H + (STATES - 1);
    strcpy(header->pet_id, "unit-test");
    strcpy(header->display_name, "Unit Test");

    pet_pkg_state_t *states = (pet_pkg_state_t *)(g_pkg + STATES_AT);
    pet_pkg_frame_t *frames = (pet_pkg_frame_t *)(g_pkg + FRAMES_AT);

    uint32_t offset = 0;
    for (int s = 0; s < STATES; s++) {
        strcpy(states[s].name, STATE_NAMES[s]);
        states[s].first = (uint16_t)(s * PER_ROW);
        states[s].count = PER_ROW;

        for (int i = 0; i < PER_ROW; i++) {
            pet_pkg_frame_t *frame = &frames[s * PER_ROW + i];
            frame->offset = offset;
            frame->x = (int16_t)(STAGE_X0 + i * 2);
            frame->y = (int16_t)(STAGE_Y0 + s);
            frame->w = FRAME_W;
            frame->h = FRAME_H;
            frame->duration = (uint16_t)(100 + i);
            g_frame_offset[s * PER_ROW + i] = (uint16_t)offset;
            offset += FRAME_BYTES;
        }
    }

    // 像素数据随便填, 只要能被 CRC 覆盖到。
    for (int i = 0; i < FRAMES * FRAME_BYTES; i++) {
        g_pkg[BLOB_AT + i] = (uint8_t)(i * 7 + 3);
    }
}

// 重算三个 CRC。改坏包之后要先调用它, 才能单独测某一条约束。
static void build_tables(void)
{
    pet_pkg_header_t *header = (pet_pkg_header_t *)g_pkg;
    header->tables_crc32 = pet_pkg_crc32(g_pkg + FRAMES_AT, BLOB_AT - FRAMES_AT);
    header->blob_crc32 = pet_pkg_crc32(g_pkg + BLOB_AT, header->blob_size);
    header->header_crc32 = 0;
    header->header_crc32 = pet_pkg_crc32(g_pkg, PET_PKG_HEADER_SIZE);
}

// header_crc32 的口径是"该字段按 0 参与", 所以先把字段清零再算整段即可。
static void rebuild_header_crc(void)
{
    pet_pkg_header_t *header = (pet_pkg_header_t *)g_pkg;
    header->header_crc32 = 0;
    header->header_crc32 = pet_pkg_crc32(g_pkg, PET_PKG_HEADER_SIZE);
}

static pet_pkg_error_t parse(pet_pkg_check_t check)
{
    pet_pkg_view_t view;
    return pet_pkg_parse(g_pkg, sizeof(g_pkg), check, &view);
}

static void test_crc32_known_vectors(void)
{
    // 与 Python binascii.crc32 一致的标准向量。
    check(pet_pkg_crc32("", 0) == 0x00000000u, "crc32 of empty string");
    check(pet_pkg_crc32("123456789", 9) == 0xCBF43926u, "crc32 of 123456789");
    check(pet_pkg_crc32("The quick brown fox jumps over the lazy dog", 43) ==
              0x414FA339u,
          "crc32 of pangram");
}

static void test_accepts_valid_package(void)
{
    build_package();
    build_tables();

    pet_pkg_view_t view;
    check(pet_pkg_parse(g_pkg, sizeof(g_pkg), PET_PKG_CHECK_BLOB_CRC, &view) ==
              PET_PKG_OK,
          "valid package parses");

    check(view.header != NULL && view.states != NULL && view.frames != NULL &&
              view.blob != NULL,
          "view points at every section");
    check(strcmp(view.header->pet_id, "unit-test") == 0, "pet id survives");
    check(view.frames == (const pet_pkg_frame_t *)(g_pkg + FRAMES_AT),
          "frame table is not copied");
    check(view.blob == g_pkg + BLOB_AT, "blob is not copied");
    check(view.states[PET_ANIM_JUMPING].first == PET_ANIM_JUMPING * PER_ROW,
          "state rows keep their order");
    check(view.frames[1].duration == 101, "frame duration survives");

    // mmap 出来的是整块分区, 比包本身大是正常情况。
    check(pet_pkg_parse(g_pkg, TOTAL_SIZE, PET_PKG_CHECK_STRUCTURE, &view) ==
              PET_PKG_OK,
          "extra trailing bytes are ignored");
    check(view.header->total_size == TOTAL_SIZE, "declared size wins over buffer size");
}

static void test_reads_header_alone(void)
{
    build_package();
    build_tables();

    // 设备只从 flash 里读 120 字节就要知道"这包有多长", 才能决定映射多少页。
    pet_pkg_header_t header;
    check(pet_pkg_read_header(g_pkg, PET_PKG_HEADER_SIZE, &header) == PET_PKG_OK,
          "header alone parses");
    check(header.total_size == TOTAL_SIZE, "header reports the package length");
    check(strcmp(header.pet_id, "unit-test") == 0, "header carries the pet id");

    // 头部之外的区域还没读进来, 所以这里不能因为"长度不够"就拒绝。
    check(pet_pkg_read_header(g_pkg, PET_PKG_HEADER_SIZE, &header) == PET_PKG_OK,
          "header parse ignores the declared length");

    // 但头部本身错了照样要拦住。
    g_pkg[offsetof(pet_pkg_header_t, total_size)] ^= 0x01;
    check(pet_pkg_read_header(g_pkg, PET_PKG_HEADER_SIZE, &header) ==
              PET_PKG_ERR_HEADER_CRC,
          "corrupt header is rejected on its own");
}

static void test_rejects_truncated_input(void)
{
    build_package();
    build_tables();

    pet_pkg_view_t view;
    check(pet_pkg_parse(g_pkg, PET_PKG_HEADER_SIZE - 1, PET_PKG_CHECK_STRUCTURE,
                        &view) == PET_PKG_ERR_SHORT,
          "short input is rejected");
    check(pet_pkg_parse(g_pkg, TOTAL_SIZE - 1, PET_PKG_CHECK_STRUCTURE, &view) ==
              PET_PKG_ERR_TOTAL_SIZE,
          "input shorter than the declared size is rejected");
}

static void test_rejects_wrong_identity(void)
{
    build_package();
    build_tables();

    pet_pkg_header_t *header = (pet_pkg_header_t *)g_pkg;
    header->magic = 0xFFFFFFFFu;   // 被擦过的分区读出来就是这个
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_MAGIC, "bad magic");

    build_package();
    header->version = PET_PKG_VERSION + 1;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_VERSION, "bad version");

    build_package();
    header->header_size = PET_PKG_HEADER_SIZE + 4;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_HEADER_SIZE,
          "bad header size");

    build_package();
    header->pet_id[0] = '\0';
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_ID, "empty pet id");

    build_package();
    memset(header->display_name, 'x', PET_PKG_NAME_MAX);  // 没有 NUL
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_ID,
          "display name without terminator");
}

static void test_rejects_corruption(void)
{
    build_package();
    build_tables();

    // 头部被改一位: 头部 CRC 必须发现。
    g_pkg[offsetof(pet_pkg_header_t, stage_h)] ^= 0x01;
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_HEADER_CRC,
          "header byte flip is caught");

    // 帧表被改一位(头部 CRC 跟着重算, 于是只剩表区 CRC 能发现)。
    build_package();
    build_tables();
    g_pkg[FRAMES_AT + 4] ^= 0x40;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_TABLES_CRC,
          "frame table byte flip is caught");

    // 状态表同样被表区 CRC 覆盖; 这是最容易被漏掉的一块 —— 它错了只会让动画
    // 静悄悄地串行。
    build_package();
    build_tables();
    g_pkg[STATES_AT + 1] ^= 0x02;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_TABLES_CRC,
          "state table byte flip is caught");

    // 像素数据被改一位: 结构检查放行, 完整检查必须拦下。
    build_package();
    build_tables();
    g_pkg[BLOB_AT + 100] ^= 0xFF;
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_OK,
          "structure check skips the blob crc");
    check(parse(PET_PKG_CHECK_BLOB_CRC) == PET_PKG_ERR_BLOB_CRC,
          "blob crc catches pixel corruption");
}

static void test_rejects_broken_state_table(void)
{
    build_package();
    build_tables();

    pet_pkg_state_t *states = (pet_pkg_state_t *)(g_pkg + STATES_AT);
    pet_pkg_frame_t *frames = (pet_pkg_frame_t *)(g_pkg + FRAMES_AT);

    // 行顺序被交换: 动画会跳到别的动作上, 只能靠名字发现。
    char saved[PET_PKG_STATE_NAME_MAX];
    memcpy(saved, states[0].name, PET_PKG_STATE_NAME_MAX);
    memcpy(states[0].name, states[1].name, PET_PKG_STATE_NAME_MAX);
    memcpy(states[1].name, saved, PET_PKG_STATE_NAME_MAX);
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_STATE_NAME,
          "swapped rows are caught");

    build_package();
    states[3].count = 1;   // 单帧动画
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_STATE_LAYOUT,
          "single-frame row is rejected");

    build_package();
    states[4].first = 0;   // 帧表不连续
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_STATE_LAYOUT,
          "non-contiguous rows are rejected");

    build_package();
    ((pet_pkg_header_t *)g_pkg)->state_count = STATES - 1;
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_STATE_COUNT,
          "state count must match pet_anim_t");

    // 帧表里出现非法几何
    build_package();
    frames[2].w = 0;
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_FRAME_GEOMETRY,
          "zero width frame is rejected");

    build_package();
    frames[2].x = PET_PKG_CELL_W - 1;   // 裁到单元格外面
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_FRAME_GEOMETRY,
          "crop outside the cell is rejected");

    build_package();
    frames[2].duration = 0;
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_FRAME_GEOMETRY,
          "missing duration is rejected");
}

static void test_rejects_broken_blob_layout(void)
{
    pet_pkg_frame_t *frames = (pet_pkg_frame_t *)(g_pkg + FRAMES_AT);

    // 帧数据越界
    build_package();
    frames[FRAMES - 1].offset = TOTAL_SIZE;   // 超出 blob
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_FRAME_RANGE,
          "frame outside the blob is rejected");

    // 两帧指向同一块数据(重叠)
    build_package();
    frames[3].offset = frames[2].offset;
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_FRAME_ORDER,
          "overlapping frames are rejected");

    // 帧数据没对齐到 4 字节: 渲染时按 uint16 读会触发异常。
    build_package();
    for (int i = 0; i < FRAMES; i++) frames[i].offset += 2;
    build_tables();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_FRAME_ORDER,
          "misaligned frame offset is rejected");
}

static void test_rejects_wrong_stage_box(void)
{
    build_package();
    build_tables();
    ((pet_pkg_header_t *)g_pkg)->stage_w += 1;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_STAGE,
          "stage box must equal the union of the crops");

    // 舞台整体偏移是最阴险的那种错: 界面不报错, 只是宠物整体偏了一点。
    build_package();
    build_tables();
    ((pet_pkg_header_t *)g_pkg)->stage_x -= 3;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_STAGE,
          "shifted stage origin is rejected");
}

static void test_rejects_impossible_counts(void)
{
    build_package();
    ((pet_pkg_header_t *)g_pkg)->frame_count = PET_PKG_FRAME_MAX + 1;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_FRAME_COUNT,
          "frame count above the compile-time cap is rejected");

    build_package();
    ((pet_pkg_header_t *)g_pkg)->total_size = (uint32_t)sizeof(g_pkg) + 1;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_TOTAL_SIZE,
          "declared size beyond the buffer is rejected");

    // 表区必须紧跟头部之后的帧表: 偏移被改乱时不能靠遍历碰运气。
    build_package();
    build_tables();
    ((pet_pkg_header_t *)g_pkg)->states_offset += 4;
    rebuild_header_crc();
    check(parse(PET_PKG_CHECK_STRUCTURE) == PET_PKG_ERR_TOTAL_SIZE,
          "detached state table is rejected");
}

int main(void)
{
    test_crc32_known_vectors();
    test_accepts_valid_package();
    test_reads_header_alone();
    test_rejects_truncated_input();
    test_rejects_wrong_identity();
    test_rejects_corruption();
    test_rejects_broken_state_table();
    test_rejects_broken_blob_layout();
    test_rejects_wrong_stage_box();
    test_rejects_impossible_counts();

    if (g_failures != 0) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("pet package tests: OK\n");
    return 0;
}
