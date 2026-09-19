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

## 构建与运行

```sh
./tools/menubar/build.sh          # 产物在 build/menubar/
./tools/menubar/build.sh --run    # 构建后直接启动
```

只依赖 Xcode Command Line Tools 里的 `swiftc`（`xcode-select --install`），
不需要 Xcode 工程，也不引入任何第三方依赖。编译期会把当前项目目录写进应用作为
默认值，所以在仓库里构建出来的应用开箱即用。

启动后图标出现在菜单栏右侧。**没有 Dock 图标是正常的**（`LSUIElement=1`）。

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

- **配置 Wi-Fi**（⌘W）：打开蓝牙配网窗口。它会预填当前这台 Mac 的 SSID 与局域网
  地址，向你要设备屏幕上显示的 4 位配对码，配对码通过后就把凭据推给设备。设备换到了
  固件里没编译进去的网络、或者设备压根还没配过网时，都用它。
  标题**不带省略号**：这个名字会原样出现在设备屏幕上，而那里的状态区只有 200px 宽，
  多一个字符就可能折行（见 `tests/test_pet_ui_text_fit.py`）。
- **桥接 / 设备 / 电量 / Codex / 气泡 / 跟随**：只读状态行。设备未连接时会提示检查是否
  在同一个 2.4G 网络。电量行在设备上报之前不显示：数值来自 CW2017 电量计、走同一条
  链路，断开时会一并清掉，避免留一个过期数字；低于等于 20% 会加警示前缀。
- **推送状态**：手工推 idle / working / waiting / ready / failed，演示和联调时
  不用切回终端。手工状态优先于日志推断，直到下一个 Codex 事件出现。
- **发送气泡文字…**（⌘T）：只改设备上的一句话，不改状态。
- **跟随 Codex 会话日志**：关掉后桥接不再读 `~/.codex/sessions`，状态只由手工
  命令驱动。
- **重启桥接**（⌘R）：重拉子进程。改了 `main/pet_config.h` 里的端口后用它生效。
- **打开日志**（⌘L）、**复制诊断信息**：出问题时前者给全量输出，后者把关键状态
  和日志尾部一次性拷进剪贴板。
- **项目目录**：如果应用不是在这个仓库里构建的，点这里重新指定。
- **退出**：会一并停掉桥接子进程，不会留下占着端口的孤儿进程。

## 无界面配网（`--provision`）

同一套蓝牙客户端也能脱离界面使用，这条路径因此可被脚本调用、也可被自动化验证：

```bash
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --scan
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --password 'hunter2'
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --forget
```

`--scan` 只列出正在广播的设备然后退出。其余情况下 `--ssid`、`--host`、`--port`
默认取当前 Wi-Fi、本机局域网地址、以及配置里的桥接端口。退出码：`0` 成功、
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
| 桥接端口 | 从 `main/pet_config.h` 的 `PET_BRIDGE_PORT` 读，改配置即改端口 |
| 控制通道 | `~/Library/Application Support/CodexPetBridge/bridge.sock` |
| 日志 | `~/Library/Application Support/CodexPetBridge/bridge.log` |
| python | 依次探测 `/usr/bin/python3`、`/opt/homebrew/bin/python3`、`/usr/local/bin/python3` |

`pet_bridge.py` 只用到标准库，所以系统自带的 python3 就够，不需要建虚拟环境。

## 防火墙

`pet_bridge.py` 会监听 `0.0.0.0:8765`，第一次运行时 macOS 可能弹出"是否允许接受
传入网络连接"。必须允许，否则设备连不上（表现为设备一直停在离线睡眠态）。
本机防火墙当前关闭时不会有这个提示。

## 分层说明

应用本身不写任何网络报文 —— 它只把命令交给 `pet_bridge.py`。因此新增的协议行为
仍然只需要在 `tools/pet_bridge.py` 里实现一次，测试
`tests/test_pet_bridge.py::test_control_channel_drives_the_device` 会覆盖
"命令确实走到了设备"这条链路。
