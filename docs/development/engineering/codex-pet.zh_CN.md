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
- 屏幕电源：空闲超过 `PET_SCREEN_IDLE_MS` 就熄屏；收到 Bridge 消息、按下按键或链路恢复都会自动亮屏。见[屏幕电源](#屏幕电源)。
- 槽里还没有宠物时，舞台区居中显示「还没有宠物」，站台上写「在菜单栏应用里安装宠物」——
  版式与装了参照宠物时完全一致，所以"空"看起来是一个状态，而不是没启动。

按键：

| 操作 | 行为 |
| --- | --- |
| 上 短按 | 本机播一次挥手 |
| 下 短按 | 本机播一次跳跃 |
| 确定 短按 | 播一次跳跃，并给 Mac 发一条 `poke`；信息面板打开时则关闭面板 |
| 确定 长按 | 打开/再次长按关闭信息面板（固件版本、宠物包、链路、网络、配对端、电量） |
| 上 长按 | 进入 BSP 驱动参考示例的演示菜单；在菜单里主菜单长按「确定」返回宠物 |

## 屏幕电源

两次 Codex 之间屏幕上没有任何变化，与其让背光一直烧着这块 520 mAh 的电芯，不如黑掉。规则与各自的理由：

- **空闲即息屏**：`PET_SCREEN_IDLE_MS`（默认 60 秒）内没有任何活动，背光归零。面板显存不掉，所以亮回来是瞬时的，不需要重绘整屏。
- **有新内容就亮**：Bridge 下发的状态/文本、任意按键、链路恢复，都会点亮屏幕并重新计时。
- **保活 ping 不算**：Bridge 每 5 秒发一条 ping（`tools/pet_bridge.py` 的 `PING_INTERVAL_S`）。把它算成活动的话，屏幕永远关不掉。
- **掉线也不算**：断连时宠物本来就在睡眠（压暗 + 放慢），而重连循环每一轮都会报一次 `CONNECTING` —— 拿它们当活动，等于断网期间屏幕反而一直被点亮。
- **必须看得见时不黑**：配网页、宠物接收页与演示菜单期间持有 hold（`pet_screen_set_hold`），且一律全亮 —— 配网页出现时设备本来就还没联网，拿链路状态去定亮度会把配对码显示在 45% 的暗屏上。释放 hold 同样算一次活动，所以传输结束时留在屏幕上的那句话（成功或失败）能拿到完整的一轮计时，而不是刚画上就被关掉。
- **黑就是黑**：息屏期间由 `pet_ui_set_anim_enabled(false)` 停掉逐帧推进 —— 每帧都要把 129x198x2 字节的精灵经 SPI 推给面板，画一块看不见的屏幕纯属浪费。

判定在 `main/pet_screen.c`（纯逻辑：`tests/test_pet_screen.c` 在主机上钉住到点时刻、hold 语义与三档背光 0 / 睡眠 / 在线）；宠物应用这边写背光的只有 `pet_app.c`，而 `pet_app.c` 里写背光的只有 `apply_screen()` 一个函数。BSP 参考演示菜单保留自己的写入口 —— 它是原样搬进来的。

## 模块划分

硬件仍在 `components/bsp`，应用全在 `main/`：

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 入口：I2C、显示/LVGL 初始化，然后交给 `pet_app` |
| `main/pet_app.c` | 应用编排：按键、电量轮询、Bridge 事件、文案覆盖自检 |
| `main/pet_ui.c` | 宠物界面与动画驱动（唯一绘制入口），以及换宠物期间的传输页 |
| `main/pet_state.c` | 链路/Codex 状态 → 动画行映射（纯逻辑） |
| `main/pet_protocol.c` | 线路协议解析（纯逻辑） |
| `main/pet_pkg.c` | `.pet` 宠物包的结构解析与自检（纯逻辑） |
| `main/pet_slot.c` | `pets` 分区：擦、写、mmap 装载与校验 |
| `main/pet_layout.c` | 版式：舞台居中 + 脚底对齐台面 + 帧落点换算（纯逻辑） |
| `main/pet_screen.c` | 屏幕电源：空闲息屏与亮屏判定、背光档位（纯逻辑） |
| `main/pet_bridge.c` | Wi-Fi STA + TCP 客户端 + 退避重连 + 宠物包接收 |
| `main/pet_settings.c` | 连接参数的 NVS 持久化 |
| `main/pet_atlas.c` | 图集访问层，把槽里那份包的帧表包成 `lv_image_dsc_t` |
| `main/pet_config.h` | 编译期默认参数（SSID / 配对端地址 / 超时 / 背光） |
| `main/pet_strings.h` | 界面文案的唯一来源（X 宏清单，供自检与测试共用） |
| `main/pet_fonts.c` | 应用字体入口（正文 16 px / 标题 20 px + fallback） |
| `main/demo_menu.c` | 原 `main.c` 的演示菜单，原样搬到独立文件 |

`pet_state.c`、`pet_protocol.c`、`pet_pkg.c`、`pet_layout.c`、`pet_screen.c` 与 `pet_strings.h` 刻意不依赖 ESP-IDF 与 LVGL，可以在主机上直接跑单元测试。

## 宠物包（`.pet`）与单槽更换

宠物**不再编进固件**。它是一份自描述的数据包，躺在独立的 `pets` 分区里，由 Mac 侧的 Pet
Bridge 经同一条 TCP 链路推上去 —— 换宠物既不重新编译，也不重新烧录。

源素材仍是 ChatGPT / Codex v2 宠物图集：1536×2288 WebP，8 列 × 11 行，单元格 192×208。
57 帧 = 9 个状态动作（idle 6、running_right 8、running_left 8、waving 4、jumping 5、failed 8、waiting 6、running 6、review 6），每帧自带毫秒时长。

打包（与设备侧 `main/pet_pkg.h` 的字节布局一一对应）：

```bash
python3 tools/gen_pet_package.py --list                    # ~/.codex/pets 里有哪些
python3 tools/gen_pet_package.py --pet sophie-portrait     # 产出 build/pets/<id>.pet
python3 tools/gen_pet_package.py --pet ~/.codex/pets/sophie --out /tmp/s.pet
```

| 偏移 | 内容 |
| --- | --- |
| `+0` | 头部 120 字节：magic `PET1`、版本、总长、帧数与状态数、舞台并集、`pet_id`、`display_name` |
| `+frames_offset` | 帧表，`frame_count × 16` 字节（offset + x/y/w/h + duration） |
| `+states_offset` | 状态表，`state_count × 20` 字节（名字 + 首帧 + 帧数） |
| `+blob_offset` | 逐帧 RGB565A8 像素 |

三块内容各有一条 CRC32：头部 / 表区（帧表与状态表连成一段）/ 像素。表区特意排成连续的
一整段，这样一条 CRC 就能同时覆盖两块 —— 状态表一旦没有校验，出问题时的表现最难从画面
上看出来。

要点：

- **逐帧紧裁**。角色只占单元格 192 px 里的 54–129 px，按非透明包围盒裁剪并记住裁剪原点，
  画面与整格完全一致，而体积少约 3 倍。
- **像素格式 RGB565A8**（2 字节颜色 + 1 字节 alpha，每像素 3 字节），LVGL 可做透明混合。`stride = w * 2`，alpha 平面紧随颜色平面，见 `lvgl/src/draw/lv_image_decoder.c` 的 `img_width_to_stride()`。
- **舞台是运行时算出来的**。头部记着所有帧裁剪区的并集（参照宠物 `sophie-portrait` 是 129×198），`pet_layout_for_stage()` 按它水平居中、竖直按脚底对齐台面反推 —— 换成更宽或更高的宠物时站台一动不动，是宠物自己站上去。尺寸超出可用范围（`main/pet_layout.h`）的包在装载时就被拒收，而不是画出一个越界的界面。
- 当前宠物 `sophie-portrait`：57 帧、2,551,288 字节（2.43 MiB）、舞台 129×198。

### 槽的语义

`pets` 分区只装得下一只（见 [firmware-layout](firmware-layout.zh_CN.md)），所以"换宠物"是
**覆写**，不是并排存放：

1. 设备先把头部擦掉再逐块写入 —— 于是"擦了一半"和"一片空槽"是同一个状态，不会有半份包
   被当成有效的。
2. 中途掉电或断网的结果是**槽空了**，设备显示「还没有宠物 / 在菜单栏应用里安装宠物」。
   没有回滚，重发一次即可。
3. 收完之前设备会核对声明的长度、整包 CRC32，再 `esp_partition_mmap` **只映射包实际占用
   的那一段**并立即 `munmap` —— 3.94 MB 整块映射会白白吃掉 MMU 页，而那些页与 app 的
   rodata 是共用的。
4. 装完之后设备重发一次 `hello`，其中的 `pet` 字段报的是槽里**真正**装上的那只。

传输期间设备上是另一块屏（见下），因为宠物界面必须先整个拆掉 —— `lv_image` 正引用着那块
要重写的 flash。

### 宠物接收页

换宠物时宠物界面必须先整个拆掉：`lv_image` 正引用着要从 flash 上 mmap 的那块内存，一边
映射一边擦是未定义行为。但拆掉之后屏幕不能黑着 —— 一份 1~3 MB 的包在 2.4 GHz Wi-Fi 上要传
十几秒到几十秒，用户看不出设备在不在干活就会去拔电，那就只剩半份包。

所以顶上这块屏：标题、宠物 id、进度条、一句状态。它不引用任何图集像素，因此可以在解除映射
之后继续显示。版式常量是 `main/pet_ui.c` 里的 `TRANS_*`（一样只写整数字面量，预览工具读它们
出图）。

- 长度未知时不假装有进度：只画外框。
- 失败的原因（包坏了、CRC 对不上、链路断了）留在屏幕上，并提示重新发送。
- 装完之后回到宠物界面；槽是空的话那里显示「还没有宠物 / 在菜单栏应用里安装宠物」，而不是
  一片黑。

## 动画驱动

`main/pet_ui.c` 里只有一个绘制入口 `render_frame()`，帧同步问题只需在这一处想清楚：

1. 每帧在图集单元格里的裁剪原点不同。要减掉的基准是**舞台在图集单元格里的裁剪原点**（包头 `stage_x/stage_y`，也就是所有帧裁剪框的并集原点），减完才是这一帧在舞台里的落点 —— 动作里的横向位移来自素材本身，与 ChatGPT 里的表现一致，不是额外加的补间。**不是**减 `pet_layout_for_stage()` 算出来的 `stage_x/stage_y`：那也是"舞台原点"，但它是舞台在**屏幕上**的落点，跟单元格坐标系差着几十像素（129 px 宽的参照宠物：单元格里 31，屏幕上 55）。两者混用不会编译失败、不会崩，只会让宠物整体左上偏移并被舞台裁掉一角 —— 真机上第一次换宠物就是这么错的，所以换算只留在 `pet_layout_frame_rect()` 这一个入口里，帧的裁剪框必须含在舞台裁剪框内，否则拒绝绘制。
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
{"type":"pet","id":"sophie-portrait","size":2551684,"crc32":2181165281}
```

设备 → Mac：

```json
{"type":"hello","fw":"0.1.0","pet":"sophie-portrait"}
{"type":"battery","soc":95}
{"type":"pong"}
{"type":"poke"}
{"type":"petdone","id":"sophie-portrait","ok":true}
```

`battery` 在电量变化时上报，`soc` 为 `null` 表示读不到；链路重连后会紧跟 `hello` 补发一次已知值，否则菜单栏在下一次电量刷新（最多 5 秒）之前只能显示未知。

`pet` 是唯一一条**后面跟着原始字节**的消息：宣告行之后的 `size` 个字节就是 `.pet` 包本身
（不 base64 —— 那要多传 33%），设备在这段时间里处于"二进制模式"，只按字节计数、不再按行
解析。所以桥接侧必须在整个传输期间持有发送锁，插进去任何一条 JSON 都会被打包体里、从此
整包错位。发完设备会回一条 `petdone` —— **在那之前只能算"发出去了"**，`ok` 为假时槽是空的。

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

这个文件**怎么找**比看上去重要：跟随循环每秒要问四次，所以只下探最近的几个日期目录（一个都拿不到才回退到递归扫描）。见 `CodexWatcher.newest_session()`。

| Codex 事件 | 宠物状态 |
| --- | --- |
| `event_msg/task_started` | `working` |
| `event_msg/item_completed`（命令/思考/改文件/消息） | `working`，并把进度当成气泡文字 |
| `event_msg/task_complete` | `ready`，气泡显示 `last_agent_message` |
| 带 `error` 的事件 | `failed` |
| 审批 / 确认类事件 | `waiting` |
| 超过 `--idle-after`（默认 45 s）没有事件 | `idle` |

服务端每 5 s 发一条 `ping`，明显快于设备侧 12 s 的空闲判定，所以「设备静默掉线」只会在真的断网时发生。

Codex 不把正文放在普通字段里：`AgentMessage` 的正文是内容块列表（`[{"type": "Text", "text": "..."}]`），报错是 `{"message": ..., "codex_error_info": ...}`。所有要变成气泡文字的 Codex 字段都走 `message_text()`：它把正文取出来，遇到不认识的结构就返回空串。它**刻意没有 `str()` 兜底** —— 把载荷序列化出去，正是那段 `[{'type': 'Text', ...}]` 顶掉进度文案、跑到宠物站台上的原因。

同一条规矩也管**条目类型本身**。`item_completed` 覆盖的类型比桥接点名的多 —— `McpToolCall`、`WebSearch`、`Plan`、`ImageView`、`ContextCompaction`、`SubAgentActivity` 等，本机 62127 条 `item_completed` 里有 2423 条落在其中 —— 认不出的一律退到「工作中」。它曾经把类型名原样发出去，于是气泡在 `McpToolCall`、`ImageView` 这类 CamelCase 英文标识符之间跳：对看着中文站台的用户毫无意义，而且随 Codex 版本增删。气泡里要么是 Codex 真正产出的正文，要么是我们自己的状态文案，没有第三条路。

### 换宠物

桥接启动时扫一遍 `~/.codex/pets`，把可用宠物的列表（`pets` 事件）随状态一起广播出去。
选中一只时它按需打包 —— 用的是 `tools/gen_pet_package.py` 的那份实现，不另写一份：包格式
是跨语言的契约，只能有一个源头 —— 缓存到 `~/Library/Application Support/CodexPetBridge/pets/<id>.pet`，
再在**后台线程**里推给设备。

- 缓存键是源图集的"文件名 + 大小 + 改动时间"：图集没动就不会重复切帧（一只 3 MB 的宠物要
  切 57 帧、逐像素转 RGB565）。
- 传输必须离开控制通道的读线程。一次推送要几十秒，卡在那里会让 GUI 看起来"点了没反应"。
- 进度走 `petxfer` 事件（`preparing` / `sending` 带已发字节 / `sent`），设备的结论走
  `petdone`。**别把 `sent` 当成成功** —— 那时设备还在核对 CRC。
- 切帧要 Pillow，而 `pet_bridge.py` 本身只用标准库 —— 所以**解释器挑错时桥接照样启动，
  只有"换宠物"会失败**。这个失败必须说清是解释器的问题：`gen_pet_package` 是惰性导入
  PIL 的（为了让 `tests/test_pet_package.py` 在没有 Pillow 的机器上也能跑），那句
  `ImportError` 不会从 `import gen_pet_package` 里抛出来，光靠兜它只会得到一句没头没脑的
  「内部错误」。现在改成先 `import PIL` 探一下，并在 `pets` 事件里带一个 `packer`
  对象（`available` / `python` / `pillow` / `hint`），好让菜单**在点之前**就把原因说清楚。
- `pets.last` 是"上一次结局"，客户端中途接入时靠它对齐进度行，所以设备给出结论后必须
  把它收成终局，并把 `petxfer` 从 `ControlHub` 的最新一份里**撤掉** —— 它是进度不是状态，
  留着最后一帧 `sent`，中途接入的客户端重放快照时（菜单栏按事件名排序合并，`petdone`
  排在 `petxfer` 前面）会把终点状态盖回「等设备确认」。同一处还要重扫一遍宠物列表：
  打包成功后缓存才刚出现，"点完还是未打包"看着像没生效。

```bash
python3 tools/pet_bridge.py --pets-dir ~/.codex/pets    # 换一批源素材
python3 tools/pet_bridge.py --pet-cache /tmp/pets       # 换个缓存目录
python3 tools/pet_bridge.py --no-pets                   # 设备没刷 pets 分区时关掉这套能力
```

### 菜单栏应用（macOS）

`tools/menubar/` 把它包成一个原生菜单栏应用：图标随状态变化，点开能看到桥接 / 设备 /
电量 / Codex / 气泡状态，并能手工推状态、发气泡文字、开关日志跟随；「宠物」子菜单列出
Codex 里可用的宠物、勾出设备上当前那只，选中即换（传输进度显示在顶栏标题里，`43%` 这样）。
菜单里的「关于」会显示桥接跑在哪个解释器上、它有没有 Pillow —— 但**没有**改它的入口：
路径打错一个字符、或者那个虚拟环境后来被删掉，现象都只是「换宠物失败」，还找不回原来那个。
挑不到带 Pillow 的解释器时，「宠物」子菜单会直接把原因写在顶上，并且禁掉那些还没打过包的
宠物，而不是让用户点一只失败一只；要把解释器指到别处，用
`defaults write local.codex-pet.bridge pythonPath <路径>`（`defaults delete` 回到自动
探测），见 `tools/menubar/README.zh_CN.md`。

```bash
./tools/menubar/build.sh --run
```

只依赖 Command Line Tools 里的 `swiftc`，没有第三方依赖，也不需要 Xcode 工程。
应用本身**不写任何网络报文** —— 它把 `pet_bridge.py` 拉成子进程（崩了按退避重拉），
再通过下面这条本地通道读状态、下命令；退出时会一并收掉子进程。

构建时会把 `pet_bridge.py` 与 `gen_pet_package.py` 复制进应用包的
`Contents/Resources/bridge/`，桥接端口也在构建期从固件头文件提取后编译进去。**应用在运行期
不读源码目录** —— 那是应用唯一一处脚本来源，所以它被搬进 `/Applications`、或者装到一台
根本没有这个仓库的机器上，都照样能跑。改完脚本或端口要重新 `build.sh`（`--install` 会连
换装带重启）。`python3` 与 Pillow 仍是外部依赖。细节见 `tools/menubar/README.md`。

### 控制通道（`--control`）

给 GUI 前端用的本地通道：AF_UNIX + JSON 行，双向。

```bash
python3 tools/pet_bridge.py --control "$HOME/Library/Application Support/CodexPetBridge/bridge.sock"
```

连上来先收到一条 `snapshot`（全部当前状态），之后是增量事件：`bridge` / `link` /
`state` / `text` / `session` / `watch` / `device` / `battery` / `pets` / `petxfer` /
`petdone` / `error`。命令有 `state`、`text`、`raw`、`watch`、`snapshot`、`ping`、
`petlist`、`pet`（带 `id`）。

`pets` / `petxfer` / `petdone` 是三类独立的设备事实，**不要合并进 `device`**：`ControlHub`
每类事件只留最新一份，混在一起会把 `hello` 报上来的固件与宠物包顶掉。同理，新增一类设备
事实就要用新的事件名。

这是一条**事件流**，不是一问一答：命令成功不回执，结果以对应事件广播出来，只有出错
才会多收到一条 `error` —— 客户端按事件更新状态即可，不要去配「发一条收一条」。
协议的权威定义仍然是上面那几条发往设备的 JSON，控制通道只做观察和驱动，不新增任何
下行报文。

## 版式预览（不需要真机）

```bash
python3 tools/preview_pet_screen.py                     # 全部屏各一张 + 一张联系表
python3 tools/preview_pet_screen.py --pet sophie         # 换一只宠物出图
python3 tools/preview_pet_screen.py --anim idle --frame 3 --scale 3
python3 tools/preview_pet_screen.py --screen nopet,transfer,prov   # 只看与宠物无关的屏
```

输出到 `assets/pets/<pet-id>/preview/screen-*.png`。除了 6 个状态屏，还有空槽、宠物接收页
（`--transfer-percent` / `--transfer-failed`）与蓝牙配网页。

这个工具**一个数字都不复制**，全部从源头读：像素与帧表来自一份 `.pet` 包（默认现打一份，
也可以用 `--package` 指一个现成的包），布局常量解析自 `main/pet_layout.{h,c}` 与
`main/pet_ui.c`，配色来自 `pet_ui.c` 的 `COL_*`，文案来自 `main/pet_strings.h`，行高与基线
来自 `assets/fonts/pet_font_{16,20}.c`。所以它同时是一道校验：打包时的裁剪偏移算错，预览里
的宠物立刻缺一块。

文字位置按 LVGL 的算法算：**基线 = 行框顶 + `line_height` - `base_line`**（见 lvgl 的
`lv_draw_label.c`）。字形用的是系统 CJK 字体（设备上是子集化的 Noto Sans CJK SC），所以
字形有细微差别，位置和字号是准的。

它把 `pet_layout_for_stage()` 在 Python 里重写了一遍（编辑器里编译不了 C），两份实现跑偏
只会让预览图骗人，所以 `tests/test_pet_layout_mirror.py` 拿设备侧的同一份 C 逐字段对照，
`tests/dump_pet_layout.c` 就是那根挂具。

对照的是**两条式子**：版式的每个字段，以及每帧的落点（`pet_layout_frame_rect()` 那一减）。
第二条是补上来的 —— 落点曾经在设备侧减错了基准（拿屏幕落点当裁剪原点），预览画得对、真机
偏了 24/33 px 并被裁掉一角，而当时的对照只覆盖版式字段，这条式子没有对手可比。

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
./tools/validate.sh --static      # 仓库检查 + 主机测试(含宠物状态机/协议/包格式/版式/字体覆盖)
PET_PYTHON=~/venv/bin/python3 ./tools/validate.sh --static   # 带 Pillow 时才跑到"图集->.pet"那一段
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
| `factory` app 分区 | 4,194,304 字节（`partitions.csv`） |
| app 镜像（不含宠物） | 3,244,544 字节（约 3.1 MiB），分区占用 77% |
| 宠物包 `.pet`（57 帧，RGB565A8） | 2.43 MiB（装在 `pets` 分区，**不在 app 里**） |
| `pets` 数据分区 | 4,128,768 字节（0x3f0000） |
| `pet_font_16` 字模（GB2312 一二级） | ~0.5 MiB |
| `pet_font_20` 字模（GB2312 一级） | ~0.4 MiB |

ESP32-C3 无 PSRAM，字模放在 Flash 里由 LVGL 直接读取（`lv_image` 引用外部 `lv_image_dsc_t`，
不复制数据）。宠物像素则是在装载时 `esp_partition_mmap` 上来的，用完立刻解除映射。LVGL 自身
的内存池仍是 `CONFIG_LV_MEM_SIZE_KILOBYTES=24`；本界面约 35 个对象，够用。

`pets` 分区只装得下一只未压缩的宠物，所以"多带几只"在 8 MB 上是做不到的 —— 那需要逐帧
deflate（一只降到 0.86~1.04 MB，但峰值要吃 75~90 KB RAM），或者砍掉 OTA 双槽。

## 资产与许可

按 `assets/README.md` 的要求记录来源与集成方式：

| 路径 | 内容 | 来源 / 许可 | 集成方式 |
| --- | --- | --- | --- |
| `assets/pets/sophie-portrait/spritesheet.webp`、`pet.json` | v2 宠物源图集 | 来自本机 `~/.codex/pets/sophie-portrait`。**上游许可未标注**，公开再分发前需自行确认 | `tools/gen_pet_package.py` 的输入，不参与编译 |
| `assets/pets/sophie-portrait/preview/screen-*.png` | 各屏的预览图 | 同上 | 仅用于人工核对，不参与编译 |
| `assets/fonts/pet_font_16.c`、`pet_font_20.c` | 生成的中文字模 | 由 `tools/gen_pet_fonts.py` 用 Noto Sans CJK SC（SIL OFL 1.1）子集化生成 | `main/CMakeLists.txt` 的 `target_sources` |
| `assets/fonts/pet_font_charset.json` | 字库码点覆盖清单 | 同上 | 供 `tests/test_pet_font_coverage.py` 读取 |

注意：

- 固件里**不再有任何宠物素材**。`main/assets/pet_*.bin` 与生成的 `pet_atlas_*.{h,c}` 都已删除，
  所以 `main/CMakeLists.txt` 里没有 `EMBED_FILES`。
- 因此**全新克隆不需要源图集就能编译**，编译出来的固件也还没有宠物：设备开机就是"还没有
  宠物"，等你在菜单栏应用里装一只。源图集只用于打包与出预览图。
- 换宠物不需要改任何源码，也不需要重新编译或烧录 —— 在菜单栏的「宠物」子菜单里选一只即可。
