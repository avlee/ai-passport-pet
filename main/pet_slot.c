// main/pet_slot.c
#include "pet_slot.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pet_slot";

static const esp_partition_t *s_partition;

static esp_partition_mmap_handle_t s_map;
static bool                        s_mounted;
static pet_pkg_view_t              s_view;

// 已装载宠物的 id 副本。不能直接从 s_view.header->pet_id 现读: 卸载时 s_mounted
// 会先置假再 memset, 而 pet_slot_pet_id() 会被 bridge 任务和 BLE 任务调用 ——
// 万一擦着读就会拿到半截字符串, 而它正是 hello / 配网信息里用来认身份的那个字段。
// 这里只在挂载成功那一刻写一次, 读侧永远是完整的。
static char s_pet_id[PET_PKG_ID_MAX] = PET_SLOT_NO_PET;

static char s_error[48] = "ok";

// --- 下载状态 ---
static bool     s_writing;
static uint32_t s_write_total;    // 包声明的总长度
static uint32_t s_write_offset;   // 已经落盘的字节数
static uint32_t s_write_crc;      // 边收边算的整包 CRC32
static uint32_t s_erased_upto;    // 已经擦到哪(64 KB 对齐)

static uint8_t s_stage[PET_SLOT_WRITE_CHUNK];
static size_t  s_stage_len;

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
}

esp_err_t pet_slot_init(void)
{
    if (s_partition != NULL) return ESP_OK;

    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           PET_SLOT_PARTITION_SUBTYPE,
                                           PET_SLOT_PARTITION_LABEL);
    if (s_partition == NULL) {
        // 分区表里没有 pets: 多半是刷了旧的分区表。宠物功能整体不可用, 但设备
        // 本身还能跑 —— 所以只报错不崩。
        ESP_LOGE(TAG, "找不到 %s 分区(data/0x%02x), 请重新烧录分区表",
                 PET_SLOT_PARTITION_LABEL, PET_SLOT_PARTITION_SUBTYPE);
        set_error("no partition");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "宠物槽: %s @ 0x%08" PRIx32 ", %" PRIu32 " KB",
             s_partition->label, s_partition->address, s_partition->size / 1024);
    return ESP_OK;
}

void pet_slot_unmount(void)
{
    // 身份先回到"没有宠物": 换宠物时校验失败要把 id 也撤回 none, 否则 hello 会
    // 一直报一只其实已经不在槽里的宠物。
    snprintf(s_pet_id, sizeof(s_pet_id), "%s", PET_SLOT_NO_PET);

    if (s_mounted) {
        esp_partition_munmap(s_map);
        s_map = 0;
        s_mounted = false;
        memset(&s_view, 0, sizeof(s_view));
    }
}

bool pet_slot_ready(void)
{
    return s_mounted;
}

const pet_pkg_view_t *pet_slot_view(void)
{
    return s_mounted ? &s_view : NULL;
}

const char *pet_slot_pet_id(void)
{
    return s_pet_id;
}

uint32_t pet_slot_capacity(void)
{
    return s_partition != NULL ? s_partition->size : 0;
}

const char *pet_slot_last_error(void)
{
    return s_error;
}

esp_err_t pet_slot_mount(void)
{
    if (s_partition == NULL) {
        set_error("no partition");
        return ESP_ERR_NOT_FOUND;
    }

    pet_slot_unmount();

    // 先只读头部: 包可能只有 3 MB, 而分区有 3.94 MB。整块映射会把 60 多个 MMU
    // 页全吃进去, 而那些页和 app 自己的 rodata 是同一份资源。
    uint8_t raw[PET_PKG_HEADER_SIZE];
    esp_err_t err = esp_partition_read(s_partition, 0, raw, sizeof(raw));
    if (err != ESP_OK) {
        set_error("read failed");
        ESP_LOGE(TAG, "读宠物槽失败: %s", esp_err_to_name(err));
        return err;
    }

    pet_pkg_header_t header;
    pet_pkg_error_t pkg_error = pet_pkg_read_header(raw, sizeof(raw), &header);
    if (pkg_error != PET_PKG_OK) {
        set_error(pet_pkg_error_name(pkg_error));
        ESP_LOGW(TAG, "槽里没有可用的宠物: %s", pet_pkg_error_name(pkg_error));
        return ESP_ERR_INVALID_STATE;
    }

    if (header.total_size > s_partition->size) {
        set_error("too large");
        ESP_LOGE(TAG, "宠物包 %" PRIu32 " 字节, 分区只有 %" PRIu32 " 字节",
                 header.total_size, s_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    const void *base = NULL;
    err = esp_partition_mmap(s_partition, 0, header.total_size,
                             ESP_PARTITION_MMAP_DATA, &base, &s_map);
    if (err != ESP_OK) {
        // 最常见的原因是 MMU 页不够: app 自己的 rodata 和宠物槽共用同一份页表,
        // 而 ESP32-C3 一共只有 128 页 x 64 KB。app 的 rodata 长大了就会挤到这里。
        set_error("mmap failed");
        ESP_LOGE(TAG, "映射宠物槽失败: %s(多半是 MMU 页不够, 见 pet_slot.c 注释)",
                 esp_err_to_name(err));
        return err;
    }

    pkg_error = pet_pkg_parse((const uint8_t *)base, header.total_size,
                              PET_PKG_CHECK_STRUCTURE, &s_view);
    if (pkg_error != PET_PKG_OK) {
        // 结构自检不过就继续跑只会画出错位的画面, 所以宁可退回"没有宠物"。
        set_error(pet_pkg_error_name(pkg_error));
        ESP_LOGE(TAG, "宠物包自检失败: %s", pet_pkg_error_name(pkg_error));
        pet_slot_unmount();
        return ESP_ERR_INVALID_STATE;
    }

    s_mounted = true;
    snprintf(s_pet_id, sizeof(s_pet_id), "%s", s_view.header->pet_id);
    set_error("ok");
    ESP_LOGI(TAG, "宠物就绪: %s (%s), %u 帧, 舞台 %ux%u, %" PRIu32 " KB",
             s_view.header->pet_id, s_view.header->display_name,
             (unsigned)s_view.header->frame_count, s_view.header->stage_w,
             s_view.header->stage_h, header.total_size / 1024);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------
// 擦除是这里最慢的一步(整槽 3.94 MB 约 20~40 秒), 所以不放在 begin 里一次性做完,
// 而是跟着写入的进度往前赶 —— 数据在链路上传的时候, 空闲的时间正好拿来擦。
// 每擦完一批让一次调度: flash 擦写期间 cache 是关着的, Wi-Fi 和 LVGL 都在等它。
static esp_err_t ensure_erased(uint32_t need)
{
    while (s_erased_upto < need) {
        if (s_erased_upto >= s_partition->size) {
            set_error("out of space");
            return ESP_ERR_INVALID_SIZE;
        }

        uint32_t chunk = PET_SLOT_ERASE_CHUNK;
        if (chunk > s_partition->size - s_erased_upto) {
            chunk = s_partition->size - s_erased_upto;
        }
        chunk &= ~(uint32_t)0xFFF;  // esp_partition_erase_range 要求 4 KB 对齐
        if (chunk == 0) {
            set_error("out of space");
            return ESP_ERR_INVALID_SIZE;
        }

        const esp_err_t err = esp_partition_erase_range(s_partition, s_erased_upto,
                                                        chunk);
        if (err != ESP_OK) {
            set_error("erase failed");
            ESP_LOGE(TAG, "擦除宠物槽失败 @ %" PRIu32 ": %s", s_erased_upto,
                     esp_err_to_name(err));
            return err;
        }
        s_erased_upto += chunk;
        vTaskDelay(1);
    }
    return ESP_OK;
}

static esp_err_t flush_stage(void)
{
    if (s_stage_len == 0) return ESP_OK;

    esp_err_t err = ensure_erased(s_write_offset + (uint32_t)s_stage_len);
    if (err == ESP_OK) {
        err = esp_partition_write(s_partition, s_write_offset, s_stage, s_stage_len);
        if (err != ESP_OK) set_error("write failed");
    }
    if (err != ESP_OK) return err;

    s_write_offset += (uint32_t)s_stage_len;
    s_stage_len = 0;
    return ESP_OK;
}

esp_err_t pet_slot_begin(uint32_t total_size)
{
    esp_err_t err = pet_slot_init();
    if (err != ESP_OK) return err;

    if (total_size <= PET_PKG_HEADER_SIZE || total_size > s_partition->size) {
        set_error("too large");
        ESP_LOGE(TAG, "宠物包 %" PRIu32 " 字节放不进 %" PRIu32 " 字节的槽", total_size,
                 s_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    // 先解除映射: 一边 mmap 一边擦写同一块 flash 是未定义行为。
    pet_slot_unmount();

    s_writing = true;
    s_write_total = total_size;
    s_write_offset = 0;
    s_write_crc = pet_pkg_crc32_begin();
    s_stage_len = 0;
    s_erased_upto = 0;

    // 头部先擦掉, 槽立刻变成"没有宠物"。这样中途掉线也不会留下一个半旧半新的包:
    // 宁可明确地什么都没有, 也不要一个看起来能用、实际帧数据已经换了一半的槽。
    err = ensure_erased(PET_PKG_HEADER_SIZE);
    if (err != ESP_OK) {
        s_writing = false;
        return err;
    }

    set_error("ok");
    ESP_LOGI(TAG, "开始接收宠物: %" PRIu32 " 字节", total_size);
    return ESP_OK;
}

esp_err_t pet_slot_write(const void *data, size_t len)
{
    if (!s_writing) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;

    const uint8_t *bytes = (const uint8_t *)data;

    while (len > 0) {
        const size_t room = sizeof(s_stage) - s_stage_len;
        const size_t take = len < room ? len : room;

        memcpy(s_stage + s_stage_len, bytes, take);
        // 整包 CRC 边收边算 —— 落盘之后不用再把 3 MB 从 flash 里读回来核一遍。
        for (size_t i = 0; i < take; i++) {
            s_write_crc = pet_pkg_crc32_step(s_write_crc, bytes[i]);
        }

        s_stage_len += take;
        bytes += take;
        len -= take;

        if (s_stage_len == sizeof(s_stage)) {
            const esp_err_t err = flush_stage();
            if (err != ESP_OK) {
                s_writing = false;
                return err;
            }
        }
    }
    return ESP_OK;
}

esp_err_t pet_slot_finish(uint32_t total_size, uint32_t crc32)
{
    if (!s_writing) return ESP_ERR_INVALID_STATE;

    esp_err_t err = flush_stage();
    if (err != ESP_OK) {
        s_writing = false;
        return err;
    }

    s_writing = false;
    const uint32_t actual_crc = pet_pkg_crc32_finish(s_write_crc);

    if (s_write_offset != total_size || total_size != s_write_total) {
        set_error("size mismatch");
        ESP_LOGE(TAG, "宠物包长度对不上: 收到 %" PRIu32 ", 声明 %" PRIu32,
                 s_write_offset, total_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (actual_crc != crc32) {
        set_error("crc mismatch");
        ESP_LOGE(TAG, "宠物包 CRC 对不上: 收到 %08" PRIx32 ", 声明 %08" PRIx32,
                 actual_crc, crc32);
        return ESP_ERR_INVALID_CRC;
    }

    set_error("ok");
    ESP_LOGI(TAG, "宠物包写入完成: %" PRIu32 " 字节", s_write_offset);
    return ESP_OK;
}

void pet_slot_abort(void)
{
    if (!s_writing) return;
    s_writing = false;
    s_stage_len = 0;
    // 头部已经是擦掉的状态, 所以槽现在就等于"没有宠物"。不需要再动 flash。
    ESP_LOGW(TAG, "宠物接收中断, 槽保持为空");
}

bool pet_slot_writing(void)
{
    return s_writing;
}

uint32_t pet_slot_written(void)
{
    if (!s_writing) return 0;
    return s_write_offset + (uint32_t)s_stage_len;
}
