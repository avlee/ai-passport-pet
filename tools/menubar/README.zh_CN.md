<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Codex Pet Bridge（macOS 菜单栏应用）

把 Mac 上的 Codex 状态推给 AI Passport 上的宠物，并在系统菜单栏上显示一个图标。

它**不是**另一份桥接实现：真正的协议实现只有 `tools/pet_bridge.py` 一份（已和设备侧
解析器做过两端互校），这个应用负责三件事：

1. 把 `pet_bridge.py` 作为子进程拉起来，管好它的生死（崩了按退避重拉）；
2. 通过 `--control` 的本地通道读取状态、下发命令；
3. 把状态画在菜单栏图标和下拉菜单上。

## 构建、安装与运行

```sh
./tools/menubar/build.sh                      # 构建，产物在 build/menubar/
./tools/menubar/build.sh --run                # 构建后直接启动（跑 build/menubar 那份）
./tools/menubar/build.sh --install            # 构建 + 装到 /Applications + 重启
./tools/menubar/build.sh --install-to <目录>   # 同上，换个安装目录
```

只依赖 Xcode Command Line Tools 里的 `swiftc`（`xcode-select --install`），
不需要 Xcode 工程，也不引入任何第三方依赖。

`--install` 是**日常那条命令**：构建、停掉正在跑的实例、替换安装位置那份、再启动。停实例时
会把桥接子进程一起收掉 —— `SIGTERM` 会跳过 AppKit 的退出流程，只杀应用会留下孤儿进程继续占着
端口，新实例就起不来了。换装前有个护栏：目标位置若已存在却不是本应用（`CFBundleIdentifier`
对不上），它会拒绝并退出，不会删掉别人的 `.app`。

`build/menubar/` 与安装位置是**两份独立副本**：重新构建只更新前者，装机那份不会跟着变。
所以改完代码要么再 `--install` 一次，要么跑的就还是旧的。启动方式：双击，或在
「系统设置 → 通用 → 登录项」里加进去；应用**不带开机自启**（没有 LaunchAgent，也没用
`SMAppService`）。

启动后图标出现在菜单栏右侧。**没有 Dock 图标是正常的**（`LSUIElement=1`）。

## 桥接脚本与端口从哪来

**构建期决定，运行期只读应用包。** 应用**不会**去读源码目录 —— 装到别人机器上没有那个目录，
它能依赖的只有包里带的东西：

| 东西 | 来源 |
| --- | --- |
| `pet_bridge.py` | 构建时由 `build.sh` 复制进 `Contents/Resources/bridge/` |
| `gen_pet_package.py` | 同上，且**必须与它同目录** |
| 桥接端口 | 构建时由 `build.sh` 从 `main/pet_config.h` 的 `PET_BRIDGE_PORT` 提取，编译进应用 |
| 版本号 | 同上，取自 `PET_FIRMWARE_VERSION` |

于是有一条要记住：**改完 `tools/pet_bridge.py`，或者改了 `PET_BRIDGE_PORT`，都要重新
`build.sh`**（装了的话用 `--install`，一条命令连换装带重启）。这是拿掉"运行期读源码目录"
必须付的代价。

两个脚本必须同目录，因为 `pet_bridge.py` 靠 `sys.path[0]` 找 `gen_pet_package`，它自己
没有任何 `sys.path` 操作。

### 为什么不让应用去仓库里找脚本

早期版本允许：菜单里能指定项目目录，仓库里那份优先于包内那份，好处是改完脚本点「重启桥接」
就生效、不必重新构建。它的代价是一个**只在作者机器上有意义的运行时开关** —— 真实用户的机器上
没有这个仓库，那一项永远是"未使用"；判断逻辑要分两种来源、要跟用户解释"现在跑的是哪一份"，
还得容忍仓库被移走。开发回路改由 `build.sh --install` 承担，它在构建期，干净得多。

包内那份是**构建期快照**，仓库里没有第二份副本：单一来源仍然是 `tools/` 下那一份，
复制这一下换来的是"应用运行不依赖仓库还在原处"。

**没被解决掉的依赖**：`python3` 与 Pillow 仍然需要 —— 前者是桥接的解释器，后者是
「换宠物」把图集打成 `.pet` 的前提。让 `.app` 连解释器也自包含是另一件事（把脚本冻成
一个可执行文件），不在这一步里。

## 菜单栏图标

| 图标 | 含义 |
| --- | --- |
| 🐾 淡显（`pawprint`） | 设备未连接 |
| 🐾 实心（`pawprint.fill`） | 设备在线，Codex 空闲 |
| ⚙️（`gearshape.2.fill`） | Codex 工作中 |
| ❗️（`exclamationmark.bubble.fill`） | Codex 等待你确认 |
| ✅（`checkmark.circle.fill`） | 本轮已完成 |
| ❌（`xmark.octagon.fill`） | 出错了 |

鼠标悬停会显示当前状态、电量与气泡文字。

## 菜单项

- **配置 Wi-Fi**（⌘W）：打开蓝牙配网窗口。它会预填本机局域网地址，向你要设备屏幕上
  显示的 4 位配对码，配对码通过后就把凭据推给设备。设备换到了固件里没编译进去的网络、
  或者设备压根还没配过网时，都用它。SSID 栏需要手动填 —— macOS 把 SSID 当定位类敏感
  数据，不给定位权限的进程读不到真名（实测 `networksetup` 只会谎报"未关联"），所以不
  做自动读取；作为补偿，**配网成功的网络会被记住**（存支持目录下 0600 的 JSON，最多
  8 条），下次打开窗口从「已保存」下拉直接选，SSID 和密码一并填好。
  标题**不带省略号**：这个名字会原样出现在设备屏幕上，而那里的状态区只有 200px 宽，
  多一个字符就可能折行（见 `tests/test_pet_ui_text_fit.py`）。
- **宠物**（子菜单）：列出 `~/.codex/pets` 里可用的宠物，勾出设备上当前那只，选中即换。
  桥接会把这只按需打包（需要 Pillow，缺了解释器时子菜单会在顶上说明原因，并禁掉尚未打过包
  的那几只），在后台线程里推给设备，进度显示在子菜单第一行与
  顶栏标题里（`传输 43%`）。确认框会提醒一句：设备是先清空槽再写的，传输中断的话设备上
  就没有宠物了，重点一次即可。列表是启动时扫的，刚在 Codex 里下了新宠物用「刷新列表」。
  换完不用重刷固件 —— 这是这套宠物包机制存在的理由（见
  `docs/development/engineering/codex-pet.zh_CN.md`）。
- **桥接 / 设备 / 电量 / Codex / 气泡 / 跟随**：只读状态行。设备未连接时会提示检查是否
  在同一个 2.4G 网络。电量行在设备上报之前不显示：数值来自 CW2017 电量计、走同一条
  链路，断开时会一并清掉，避免留一个过期数字；低于等于 20% 会加警示前缀。
- **推送状态**：手工推 idle / working / waiting / ready / failed，演示和联调时
  不用切回终端。手工状态优先于日志推断，直到下一个 Codex 事件出现。
- **发送气泡文字…**（⌘T）：只改设备上的一句话，不改状态。
- **跟随 Codex 会话日志**：关掉后桥接不再读 `~/.codex/sessions`，状态只由手工
  命令驱动。
- **重启桥接**（⌘R）：手动重拉一次子进程（它崩了本来就会被自动重拉）。改了
  `pet_bridge.py` 或桥接端口要先重新构建，见上一节 —— 运行中的子进程不会自己去读仓库。
- **打开日志**（⌘L）、**复制诊断信息**：出问题时前者给全量输出，后者把关键状态
  和日志尾部一次性拷进剪贴板。「复制诊断信息」里那条 `桥接脚本:` 是绝对路径，包不完整时
  会写明"未找到"。
- **关于 Codex Pet Bridge**：版本号、**解释器路径**、桥接脚本路径、桥接端口、控制通道。
  解释器是唯一会决定「换宠物」能不能用的东西（把图集打成 `.pet` 需要 Pillow），所以它在这里
  只读显示，菜单里**没有**改它的入口 —— 原因和改法见下面的「解释器与 Pillow」。
- **退出**：会一并停掉桥接子进程，不会留下占着端口的孤儿进程。

## 无界面配网（`--provision`）

同一套蓝牙客户端也能脱离界面使用，这条路径因此可被脚本调用、也可被自动化验证：

```bash
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --scan
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --password 'hunter2'
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --forget
```

`--scan` 只列出正在广播的设备然后退出。其余情况下 `--ssid`、`--host`、`--port`
默认取当前 Wi-Fi、本机局域网地址、以及编译进应用的桥接端口。退出码：`0` 成功、
`1` 失败、`2` 参数错误。`--json` 每行输出一个 JSON 对象；`--out <路径>` 额外把它们
追加到文件。

蓝牙权限是按**责任进程**判定的，而不是看被执行的那个二进制。从终端或自动化宿主里
拉起时，责任进程是终端本身 —— 它没有 `NSBluetoothAlwaysUsageDescription`，进程会直接
以 `SIGABRT` 死掉，终止命名空间是 `TCC`。改用 LaunchServices 启动
（`open -n … --args`）时责任进程才变成应用自己，`--out` 就是为这种情况准备的：
结果写进文件再回收。无论哪种方式，首次运行都会弹权限询问，需要有人点同意。

## 端口与路径

| 项 | 值 |
| --- | --- |
| 桥接端口 | 构建期从 `main/pet_config.h` 的 `PET_BRIDGE_PORT` 提取后编译进应用；改那个宏要重新构建 |
| 桥接脚本 | **只有一处**：应用包内 `Contents/Resources/bridge/pet_bridge.py`（构建期从 `tools/` 复制） |
| 数据目录 | `~/Library/Application Support/CodexPetBridge/`（socket、日志、宠物缓存） |
| 控制通道 | `~/Library/Application Support/CodexPetBridge/bridge.sock` |
| 日志 | `~/Library/Application Support/CodexPetBridge/bridge.log` |
| python | 自动探测 `/opt/homebrew/bin/python3` → `/usr/local/bin/python3` → `/usr/bin/python3`，优先挑能 `import PIL` 的那个；**界面里不能改**，要覆盖见「解释器与 Pillow」 |

`pet_bridge.py` 只用到标准库，所以系统自带的 python3 就够，不需要建虚拟环境。

卸载就是删掉 `Codex Pet Bridge.app`；上面的数据目录与偏好域 `local.codex-pet.bridge`
会留下，要清干净得自己删。

## 解释器与 Pillow

桥接跑在哪个 `python3` 上，**菜单里没有修改入口**，只在「关于」里只读显示。原因是它太容易
被改坏：路径打错一个字符、或者那个虚拟环境后来被删掉，现象都只是「换宠物失败」，而改坏之后
也没法在界面里找回原来的那个。

不手动指定时按上表顺序探测，**优先挑能 `import PIL` 的那个**。`pet_bridge.py` 本体只用标准库，
所以没有 Pillow 的解释器照样能把桥接跑起来 —— 只有「换宠物」那一步需要 Pillow（要把图集打成
设备收的 `.pet`）。这种「一半能用」最难查，所以「宠物」子菜单会在点之前就在顶上写明原因，
并禁掉还没打过包的那几只。

万一带 Pillow 的解释器不在上面那三个位置（比如装在某个虚拟环境里 —— 本机就是这样），
用命令行指定：

```bash
defaults write local.codex-pet.bridge pythonPath /path/to/python3   # 指定
defaults delete local.codex-pet.bridge pythonPath                   # 回到自动探测
```

**指到不可执行的路径不会让桥接起不来**：那一条会被忽略、回落自动探测，「关于」与「复制诊断
信息」里都会写明「手动指定的路径不可执行，已回落自动探测」。

## 防火墙

`pet_bridge.py` 会监听 `0.0.0.0` 上的桥接端口（默认 8765），第一次运行时 macOS 可能弹出
"是否允许接受传入网络连接"。必须允许，否则设备连不上（表现为设备一直停在离线睡眠态）。
本机防火墙当前关闭时不会有这个提示。

## 分层说明

应用本身不写任何网络报文 —— 它只把命令交给 `pet_bridge.py`。因此新增的协议行为
仍然只需要在 `tools/pet_bridge.py` 里实现一次，测试
`tests/test_pet_bridge.py::test_control_channel_drives_the_device` 会覆盖
"命令确实走到了设备"这条链路。

运行期只有**一个**脚本来源（应用包内），所以不存在"现在跑的是哪一份"这个问题：数据目录里
不放脚本，仓库也不参与。仓库与运行期的唯一连接点就是 `build.sh`，它负责把脚本复制进包、
把端口编译进去 —— 都在构建期完成。
