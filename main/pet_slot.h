// main/pet_slot.h
// 宠物槽: `pets` 数据分区上的那一份宠物包, 以及换一份新包的全过程。
//
// 为什么不是"编进 app": 8 MB Flash 已经被 app 吃满, app 槽里剩下的 2.42 MB 比
// 最瘦的一只宠物还少 15 KB —— 想多带一只都不可能。把宠物挪出 app 之后 app 只要
// 约 3.1 MB, 于是能划出 4 MB 的 app + 3.94 MB 的宠物槽。
//
// 槽里存的是 mmap 出来的只读 flash, 所以每帧像素是零拷贝: LVGL 直接读那块地址,
// 不复制、不占 RAM。代价是**写槽之前必须先解除映射** —— 一边 mmap 一边擦写同一块
// flash 是未定义行为。
//
// 换宠物的完整流程:
//
//   pet_slot_begin(total)        解除映射, 擦掉头部(槽立即变成"没有宠物")
//   pet_slot_write(...)          边收边擦边写
//   pet_slot_finish(total, crc)  校验长度与整包 CRC
//   pet_slot_mount()             重新映射并自检, 成功后 pet_slot_ready() 为真
//
// 中途掉线就留在"没有宠物"的状态: 单槽装不下新旧两份, 所以覆盖写没法回滚。
// 这是这个方案的已知代价, 上层(菜单栏应用)看到 pet id 变成 none 会自动重推。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pet_pkg.h"

// 分区表里的标签与子类型, 见根目录 partitions.csv。
#define PET_SLOT_PARTITION_LABEL "pets"
#define PET_SLOT_PARTITION_SUBTYPE 0x40

// 没有可用宠物时 pet_slot_pet_id() 的返回值。也是 hello 报文里 pet 字段的取值。
#define PET_SLOT_NO_PET "none"

// 擦除的批大小。esp_flash 在每擦完一个批之后会松开总线并重新开 cache, 所以这个
// 大小同时决定两件事: 单批要关多久 cache(Wi-Fi/LVGL 都在等), 以及多久让出一次
// CPU。16 KB = 4 个扇区, 实测单批约 0.2 秒 —— 再大就会让 pet_bridge_stop() 的
// 2 秒等待窗口兜不住, 配网切网络时可能出现"任务没在预期时间内退出"。
#define PET_SLOT_ERASE_CHUNK 16384
// 写入的暂存大小。TCP 收到的分片长度是任意的, 攒够再落盘能少很多次 flash 调用。
#define PET_SLOT_WRITE_CHUNK 4096

// 找到 pets 分区。可重复调用, 幂等。
esp_err_t pet_slot_init(void);

// 读取并校验槽里的包, 然后把它映射进地址空间。可重复调用(会先解除上一次映射)。
// 槽里没有合法宠物时返回非 ESP_OK, 且 pet_slot_ready() 为假 —— 这不是致命错误,
// 设备照常运行, 只是界面显示"还没有宠物"。
esp_err_t pet_slot_mount(void);

// 解除映射。没有映射时是空操作。
void pet_slot_unmount(void);

bool pet_slot_ready(void);

// 已挂载的宠物包视图。未挂载时返回 NULL。
const pet_pkg_view_t *pet_slot_view(void);

// 当前宠物的 id。没有可用宠物时返回 PET_SLOT_NO_PET。
const char *pet_slot_pet_id(void);

// 分区总字节数(0 表示还没找到分区)。菜单栏/协议用它告诉用户还剩多少余量。
uint32_t pet_slot_capacity(void);

// 最近一次失败的原因(简短英文, 可直接放进协议回执)。没有失败过时返回 "ok"。
const char *pet_slot_last_error(void);

// --- 换宠物 ---------------------------------------------------------------
// 开始接收新宠物。会先解除映射, 再把头部所在扇区擦掉, 于是槽立刻变成无效状态。
// total_size 是包声明的总长度, 会先跟分区容量对一次。
esp_err_t pet_slot_begin(uint32_t total_size);

// 追加一段收到的数据。没有下载在跑时返回 ESP_ERR_INVALID_STATE。
esp_err_t pet_slot_write(const void *data, size_t len);

// 收尾: 落盘暂存、核对长度与整包 CRC32。失败时槽保持无效。
esp_err_t pet_slot_finish(uint32_t total_size, uint32_t crc32);

// 放弃这次下载(超时/链路断开)。槽保持无效, 已写入的内容不再有意义。
void pet_slot_abort(void);

// 是否正在接收。
bool pet_slot_writing(void);

// 本次接收已经落盘的字节数(含还没刷出去的暂存)。没在接收时返回 0。
// 界面的进度条用它而不是用"链路上收到多少": 擦除会把写入拖慢, 按落盘算才不会
// 出现"进度条跑满但还没写完"。
uint32_t pet_slot_written(void);
