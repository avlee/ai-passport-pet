<p align="right">
  <strong>简体中文</strong> · <a href="codex-pet.md">English</a>
</p>

# Codex 宠物（Codex Pet）

把 AI Passport 当成 Codex / ChatGPT 桌面版那只小宠物的**第二块屏幕**：屏幕上的动作、状态与文字随 Mac 上的 Codex 实时变化，断连时宠物进入睡眠。

这是本仓库 `feature/codex-pet` 分支的应用，不改变 `components/bsp` 的对外接口。

## 它长什么样

- 宠物舞台按图集原始像素 1:1 绘制，不缩放；舞台不画底色，宠物直接立在背景上，「地面」由站台和脚底接触阴影交代。
- 顶部一行：左侧状态圆点 + 状态文字（离线 / 连接中 / 空闲 / 工作中 / 等待确认 / 已完成 / 出错了），右侧电量。
- 底部一座站台：台身正面承载 Codex 下发的文本，没有文本时显示当前状态的占位文案；台面前沿压在宠物脚底那一行，所以宠物读起来是「站在站台上」。
- 断连（睡眠）：宠物整体压暗到 40%，帧速放慢到 250%，背光降到 45%，舞台右上角出现 `Zzz`。

按键：

| 操作 | 行为 |
| --- | --- |
| 上 短按 | 本机播一次挥手 |
| 下 短按 | 本机播一次跳跃 |
| 确定 短按 | 播一次跳跃，并给 Mac 发一条 `poke`；信息面板打开时则关闭面板 |
| 确定 长按 | 打开/再次长按关闭信息面板（固件版本、宠物包、链路、网络、配对端、电量） |
| 上 长按 | 进入 BSP 驱动参考示例的演示菜单；在菜单里主菜单长按「确定」返回宠物 |

## 模块划分

硬件仍在 `components/bsp`，应用全在 `main/`：

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 入口：I2C、显示/LVGL 初始化，然后交给 `pet_app` |
| `main/pet_app.c` | 应用编排：按键、电量轮询、Bridge 事件、文案覆盖自检 |
| `main/pet_ui.c` | 宠物界面与动画驱动（唯一绘制入口） |
| `main/pet_state.c` | 链路/Codex 状态 → 动画行映射（纯逻辑） |
| `main/pet_protocol.c` | 线路协议解析（纯逻辑） |
| `main/pet_bridge.c` | Wi-Fi STA + TCP 客户端 + 退避重连 |
| `main/pet_settings.c` | 连接参数的 NVS 持久化 |
| `main/pet_atlas.c` | 图集访问层，把帧表包成 `lv_image_dsc_t` |
| `main/pet_atlas_validate.c` | 图集自洽校验（纯逻辑） |
| `main/pet_config.h` | 编译期默认参数（SSID / 配对端地址 / 超时 / 背光） |
| `main/pet_strings.h` | 界面文案的唯一来源（X 宏清单，供自检与测试共用） |
| `main/pet_fonts.c` | 应用字体入口（正文 16 px / 标题 20 px + fallback） |
| `main/demo_menu.c` | 原 `main.c` 的演示菜单，原样搬到独立文件 |

`pet_state.c`、`pet_protocol.c`、`pet_atlas_validate.c`、`pet_strings.h` 刻意不依赖 ESP-IDF 与 LVGL，可以在主机上直接跑单元测试。

## 宠物图集（v2 格式）

素材是 ChatGPT / Codex v2 宠物图集：1536×2288 WebP，8 列 × 11 行，单元格 192×208。57 帧 = 9 个状态动作（idle 6、running_right 8、running_left 8、waving 4、jumping 5、failed 8、waiting 6、running 6、review 6），每帧自带毫秒时长。

生成固件资源：

```bash
python3 tools/gen_pet_assets.py \
    --atlas assets/pets/<pet-id>/spritesheet.webp \
    --pet-id <pet-id> \
    --out-bin main/assets/pet_<pet>_portrait.bin \
    --out-header main/pet_atlas_<pet>_portrait.h \
    --out-source main/pet_atlas_<pet>_portrait.c \
    --preview-dir assets/pets/<pet-id>/preview
```

要点：

- **逐帧紧裁**。角色只占单元格 192 px 里的 54–129 px，按非透明包围盒裁剪再加回偏移，画面与整格完全一致，Flash 少用约 3 倍。
- **像素格式 RGB565A8**（2 字节颜色 + 1 字节 alpha，每像素 3 字节），LVGL 可做透明混合。`stride = w * 2`，alpha 平面紧随颜色平面，见 `lvgl/src/draw/lv_image_decoder.c` 的 `img_width_to_stride()`。
- 当前宠物 `sophie-portrait`：57 帧、2,551,288 字节（2.43 MiB）、绘制外框 129×198。

## 动画驱动

`main/pet_ui.c` 里只有一个绘制入口 `render_frame()`，帧同步问题只需在这一处想清楚：

1. 每帧在图集单元格里的裁剪原点不同。扣掉舞台原点（`PET_ATLAS_STAGE_X/Y`）后设给 `lv_image` 的位置，才是它在屏上的落点 —— 动作里的横向位移来自素材本身，与 ChatGPT 里的表现一致，不是额外加的补间。
2. 下一帧的间隔直接取该帧的 `duration`，不插值、不固定帧率。睡眠时乘 `PET_STATE_SLEEP_SPEED_PCT`。
3. 一次性动作（连上时的挥手、完成时的跳跃庆祝、按键互动）播完自动回落：状态机过场由 `pet_state_oneshot_complete()` 推进，用户触发的一次性动作由 UI 的 override 标志接管。
4. 帧率完全由素材决定（110–320 ms，约 3–9 fps）。整块舞台 129×198 ≈ 全屏 33%，在 40 MHz SPI + 240×20 DMA 缓冲下有几倍余量。

### 状态 → 动作映射

| 链路 | Codex 状态 | 循环动作 | 备注 |
| --- | --- | --- | --- |
| offline | — | `idle` | 睡觉：压暗 40%，帧速 250%；此期间忽略 Codex 状态 |
| connecting | — | `waiting` | 正在连 Wi-Fi / TCP |
| online | idle | `idle` | |
| online | working | `running` | 原地干活 |
| online | waiting | `waiting` | 等你确认 |
| online | ready | `review` | 先播一轮 `jumping` 庆祝 |
| online | failed | `failed` | |

刚连上时会先播一轮 `waving` 打招呼，再落到当前状态对应的循环动作。

## 线路协议

TCP 上是 UTF-8 文本，一行一条消息，内容是扁平 JSON。行尾 `\n` 或 `\r\n` 都接受。

Mac → 设备：

```json
{"type":"state","state":"working","text":"正在重构登录模块"}
{"type":"text","text":"只更新文本, 不改状态"}
{"type":"ping"}
```

设备 → Mac：

```json
{"type":"hello","fw":"0.1.0","pet":"sophie-portrait"}
{"type":"battery","soc":95}
{"type":"pong"}
{"type":"poke"}
```

`battery` 在电量变化时上报，`soc` 为 `null` 表示读不到；链路重连后会紧跟 `hello` 补发一次已知值，否则菜单栏在下一次电量刷新（最多 5 秒）之前只能显示未知。

解析是**有界且宽容**的：单行上限 `PET_PROTOCOL_LINE_MAX`（512 字节）、文本字段上限 `PET_PROTOCOL_TEXT_MAX`（192 字节），超长行丢弃并在下一个换行处重新同步，未知字段忽略，UTF-8 只在字符边界上截断。设备是 TCP 客户端、Mac 是服务端，所以设备侧不需要接受入站连接，也不依赖 mDNS 发现。

## Mac 侧：Pet Bridge

`tools/pet_bridge.py` 是配套的服务端，把 Codex 的状态喂给设备：

```bash
# 跟随最新被改动的 Codex 会话日志, 并允许在同一个终端手工驱动
python3 tools/pet_bridge.py

# 只监听, 状态全靠手工命令
python3 tools/pet_bridge.py --no-codex

# 看看会跟随哪个会话文件
python3 tools/pet_bridge.py --list-sessions
```

它会跟随 `~/.codex/sessions/**/rollout-*.jsonl` 里最近被改动的那个文件，把事件映射成状态：

| Codex 事件 | 宠物状态 |
| --- | --- |
| `event_msg/task_started` | `working` |
| `event_msg/item_completed`（命令/思考/改文件/消息） | `working`，并把进度当成气泡文字 |
| `event_msg/task_complete` | `ready`，气泡显示 `last_agent_message` |
| 带 `error` 的事件 | `failed` |
| 审批 / 确认类事件 | `waiting` |
| 超过 `--idle-after`（默认 45 s）没有事件 | `idle` |

服务端每 5 s 发一条 `ping`，明显快于设备侧 12 s 的空闲判定，所以「设备静默掉线」只会在真的断网时发生。

### 菜单栏应用（macOS）

`tools/menubar/` 把它包成一个原生菜单栏应用：图标随状态变化，点开能看到桥接 / 设备 /
电量 / Codex / 气泡状态，并能手工推状态、发气泡文字、开关日志跟随。

```bash
./tools/menubar/build.sh --run
```

只依赖 Command Line Tools 里的 `swiftc`，没有第三方依赖，也不需要 Xcode 工程。
应用本身**不写任何网络报文** —— 它把 `pet_bridge.py` 拉成子进程（崩了按退避重拉），
再通过下面这条本地通道读状态、下命令；退出时会一并收掉子进程。细节见
`tools/menubar/README.md`。

### 控制通道（`--control`）

给 GUI 前端用的本地通道：AF_UNIX + JSON 行，双向。

```bash
python3 tools/pet_bridge.py --control "$HOME/Library/Application Support/CodexPetBridge/bridge.sock"
```

连上来先收到一条 `snapshot`（全部当前状态），之后是增量事件：`bridge` / `link` /
`state` / `text` / `session` / `watch` / `device` / `battery` / `error`。命令有
`state`、`text`、`raw`、`watch`、`snapshot`、`ping`。

这是一条**事件流**，不是一问一答：命令成功不回执，结果以对应事件广播出来，只有出错
才会多收到一条 `error` —— 客户端按事件更新状态即可，不要去配「发一条收一条」。
协议的权威定义仍然是上面那几条发往设备的 JSON，控制通道只做观察和驱动，不新增任何
下行报文。

## 版式预览（不需要真机）

```bash
python3 tools/preview_pet_screen.py          # 每个状态一张 + 一张拼起来的联系表
python3 tools/preview_pet_screen.py --anim idle --frame 3 --scale 3
```

输出到 `assets/pets/<pet-id>/preview/screen-*.png`。

这个工具**从生成产物画**，而不是从源图集画：帧表解析自 `main/pet_atlas_<pet>_portrait.c`，
像素读自 `main/assets/pet_<pet>_portrait.bin`，也就是固件真正烧进去的那份字节；布局常量
从 `main/pet_ui.c` 解析。所以它同时是一道校验 —— 如果裁剪偏移或 blob 布局算错，预览里的
宠物会立刻缺一块或错位。文字用系统 CJK 字体近似（设备上用的是子集化的 Noto Sans CJK SC），
位置和字号是准的，字形会有细微差别。

## 配置、构建与烧录

连接参数在 `main/pet_config.h`。不想把凭据写进被 git 跟踪的文件时，新建 `main/pet_config_local.h` 覆盖同名宏（已在 `.gitignore` 里）：

```c
#define PET_WIFI_SSID "your-2.4g-ssid"   // ESP32-C3 没有 5 GHz 射频
#define PET_WIFI_PASSWORD "your-password"
#define PET_BRIDGE_HOST "192.168.1.10"   // Mac 的局域网 IP: ipconfig getifaddr en0
#define PET_BRIDGE_PORT 8765
```

这组值只在编译期作为默认值使用；NVS 里存在一整套显式覆盖时才以 NVS 为准，且**不会**把默认值写回 NVS（否则改完 `pet_config.h` 重新烧录后旧的 SSID 还会生效）。

这里完全可以不填真实 SSID。留占位符不动，设备开机就会以「未配网」状态起来，转而提供蓝牙配对 —— 见[蓝牙配网](#蓝牙配网)。

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
./tools/validate.sh --static      # 仓库检查 + 主机测试(含宠物状态机/协议/图集/字体覆盖)
./tools/validate.sh --firmware    # 构建 + 校验 + 产出合并镜像
./tools/validate.sh               # 全部

idf.py -B build flash             # 常规烧录
esptool.py -p /dev/tty.usbmodem* write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

合并镜像在 `build/FoloToy-AI-Passport-full.bin`，从 `0x0` 一次写入即可（bootloader + 分区表 + 应用都已在里面）。

## 蓝牙配网

`pet_config_local.h` 里的值是编译期常量：换个网络就得改文件、重新构建、重新烧录。设备也可以改为通过蓝牙从菜单栏应用接收 Wi-Fi 参数 —— 那些参数本来就躺在它正在运行的那台 Mac 上。

各部分如何衔接：

- 固件出厂带着占位 SSID（`PET_WIFI_PLACEHOLDER_SSID`）。`pet_settings_is_configured()` 把占位符视为「从未配过网」，设备因此会在开机时打开配网窗口并开始广播。在 `pet_config_local.h` 里填了真实 SSID 的用法不受影响，仍直接联网；两种情况都可以长按 **下键** 开关配网窗口 —— 它是**开关**：窗口开着时再长按下键就退出，与屏幕底部「长按下键退出配网」的提示一致。配网页不透明、盖住整个画面，且期间其余按键被刻意忽略；所以一个关不掉的配网页会被直接读成「设备死机」——这条提示语和处理分支必须成对改（`tests/test_pet_button_semantics.py` 钉住了它们）。
- 屏幕上显示 4 位配对码。它是**长期**的：首次生成后写进 NVS，以后开窗、重连、重启都用同一个（`pet_settings_load_pin()`）。只有连续错满 `PET_PROVISION_MAX_ATTEMPTS` 次才换一个新码并断开链路 —— 那已经不是手滑，而是有人在试。配对码要防的只是「旁边别的机器顺手连上」，屏幕上一直写着它，做成一次一换只会让用户每次都得跑到设备跟前重抄。
- 尝试次数**不随连接重置**。稳定的码配上「每次连接白送三次机会」等于可以把码慢慢试出来；次数在开窗、配对成功、换码时才回到上限。
- 只有配对码通过之后设备才会写入任何东西。命令是单键 JSON 行 —— `{"pin":"1234"}`、`{"ssid":"…"}`、`{"pass":"…"}`、`{"host":"…"}`、`{"port":8765}`、`{"commit":true}`、`{"forget":true}` —— 逐字段下发，这样每个字段都能单独确认，失败时也能指到具体是哪一个。
- `{"commit":true}` 会写入 NVS 并重建 Bridge 链路，然后把设备拿到的 IP 回报回来。密码不会出现在任何状态报文里。成功后设备停留 `PROV_CLOSE_AFTER_DONE_MS`（3 秒）展示拿到的地址，**然后自动退出配网界面**回到宠物；失败则把错误留在屏幕上，等窗口超时或用户重试。收尾必须走 `pet_provision_stop()`：只删任务的话 `s_running` 仍是真、界面也收不到 `PET_PROV_OFF`，配网画面会一直盖在宠物上面。
- `{"forget":true}` 清空 NVS 里的**连接参数**，设备回到未配网状态。配对码不在清空范围内 —— 它管的是「哪台机器能连上设备」，跟网络参数是两件事。

配对码是应用层的校验，不是蓝牙层的加密：它只能拦住旁边顺手推凭据的人，仅此而已。请把配网当作可信局域网里的便利功能，而不是一套授权方案。

`pet_provision_parse.c` 承载解析与状态拼装逻辑，刻意不依赖 NimBLE，所以 `tests/test_pet_provision.c` 能在主机上覆盖它 —— 命令分帧、字段长度上限、以及各种拒绝路径。这一层已经抓出过两个 bug：状态拼装把自己 JSON 的结构引号也转义了；以及 UUID 解析排在了 `nimble_port_init()` 之前（见下）。

Mac 侧用 CoreBluetooth 与设备通信（`tools/menubar/Sources/ProvisionClient.swift`），应用因此保持零第三方依赖。首次使用会触发 macOS 的蓝牙权限询问。

有两处调用顺序容易写错：

- UUID 解析必须在 `nimble_port_init()` **之后**。`ble_uuid_from_str()` 会走到 `ble_uuid_base_init()`，后者要用 NimBLE 的分配器申请 16 字节；提前调用会返回 `BLE_HS_ENOMEM`，表现为四个 UUID 全部「解析失败」——而且是时好时坏，看起来像 UUID 字符串写错了，实际是调用顺序问题。
- `pet_settings_load()` 必须排在配网窗口打开之前，因为窗口要根据装载出来的 SSID 判断这台设备是不是还没配过网。

## 中文字体

基线 LVGL 只带 Montserrat，没有任何汉字，所以中文界面必须自备字库：

- `main/pet_strings.h` 是界面文案的唯一来源，用 X 宏列成清单。
- `tools/gen_pet_fonts.py` 生成 `assets/fonts/pet_font_16.c`（GB2312 一二级，7029 码点）与 `pet_font_20.c`（一级，4021 码点）以及覆盖清单 `pet_font_charset.json`。
- 两道覆盖关卡：开机时 `pet_app.c` 把清单逐条查字形并打日志；主机上 `tests/test_pet_font_coverage.py` 对照清单核对。新增文案只改 `pet_strings.h`，两处自动跟上。

详见 [lvgl-chinese-fonts.zh_CN.md](lvgl-chinese-fonts.zh_CN.md)。

## 资源占用

| 项 | 大小 |
| --- | --- |
| 宠物图集 blob（RGB565A8，57 帧） | 2.43 MiB |
| `pet_font_16` 字模（GB2312 一二级） | ~0.5 MiB |
| `pet_font_20` 字模（GB2312 一级） | ~0.4 MiB |
| `factory` app 分区 | 8,323,072 字节（`partitions.csv`） |

ESP32-C3 无 PSRAM，图集与字模都放在 Flash 里由 LVGL 直接读取（`lv_image` 引用外部 `lv_image_dsc_t`，不复制数据）。LVGL 自身的内存池仍是 `CONFIG_LV_MEM_SIZE_KILOBYTES=24`；本界面约 35 个对象，够用。

## 资产与许可

按 `assets/README.md` 的要求记录来源与集成方式：

| 路径 | 内容 | 来源 / 许可 | 集成方式 |
| --- | --- | --- | --- |
| `assets/pets/sophie-portrait/spritesheet.webp`、`pet.json` | v2 宠物源图集 | 来自本机 `~/.codex/pets/sophie-portrait`。**上游许可未标注**，公开再分发前需自行确认 | 生成脚本的输入，不参与编译 |
| `assets/pets/sophie-portrait/preview/*.png` | 每个动作一帧的预览图 | 同上 | 仅用于人工核对，不参与编译 |
| `assets/fonts/pet_font_16.c`、`pet_font_20.c` | 生成的中文字模 | 由 `tools/gen_pet_fonts.py` 用 Noto Sans CJK SC（SIL OFL 1.1）子集化生成 | `main/CMakeLists.txt` 的 `target_sources` |
| `assets/fonts/pet_font_charset.json` | 字库码点覆盖清单 | 同上 | 供 `tests/test_pet_font_coverage.py` 读取 |
| `main/assets/pet_sophie_portrait.bin` | 逐帧裁剪后的 RGB565A8 帧数据 | 由 `assets/pets/` 的源图集派生 | `main/CMakeLists.txt` 的 `EMBED_FILES` |
| `main/pet_atlas_sophie_portrait.{h,c}` | 生成的帧表 | 同上 | 编译进固件 |

注意两点：

- 生成产物（`.bin` / 生成的 `.c` / `.h` / 清单）都已提交，因此**全新克隆不需要源图集也能编译**；源图集只用于重新生成或换宠物。
- 换宠物时需要同步改三处：`main/pet_atlas.h` 里包含的生成头文件、`main/pet_atlas.c` 里的 `extern` 符号名、`main/CMakeLists.txt` 的 `EMBED_FILES` 路径。
