// tools/menubar/Sources/StatusMenuController.swift
//
// 菜单栏图标与下拉菜单。图标随设备/Codex 状态变化, 菜单是把 BridgeSupervisor 的
// 快照"念"出来, 另外提供几个手工驱动项。

import AppKit

final class StatusMenuController: NSObject {
    private let statusItem = NSStatusBar.system.statusItem(
        withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private var supervisor: BridgeSupervisor

    // 菜单一次性建好再改标题, 不走"每次事件重建整个菜单" —— 后者在菜单正打开时
    // 会让高亮跳动。
    private let bridgeItem = NSMenuItem()
    private let deviceItem = NSMenuItem()
    private let batteryItem = NSMenuItem()
    private let codexItem = NSMenuItem()
    private let bubbleItem = NSMenuItem()
    private let sessionItem = NSMenuItem()
    private let errorItem = NSMenuItem()
    private let watchItem = NSMenuItem()

    /// 宠物子菜单。与顶层菜单相反, 这一份是每次按需重建的 —— 它平时是合上的, 不会
    /// 出现"菜单开着重建导致高亮乱跳"的问题, 而内容(列表 + 进度行)本来就是活的。
    private let petItem = NSMenuItem(title: "宠物", action: nil, keyEquivalent: "")
    private let petMenu = NSMenu()
    /// 上次重建子菜单时的内容指纹。传输期间 petxfer 每 16 KiB 就报一次进度, 没必要
    /// 每次都把 NSMenuItem 重造一遍。
    private var petMenuKey = ""

    private let manualStates: [(id: String, label: String)] = [
        ("idle", "空闲"),
        ("working", "工作中"),
        ("waiting", "等待确认"),
        ("ready", "已完成"),
        ("failed", "出错了"),
    ]

    /// 配网窗口按需创建: 没配过网的人不该为这个功能付启动成本。
    private lazy var provisionController = ProvisionWindowController()

    init(supervisor: BridgeSupervisor) {
        self.supervisor = supervisor
        super.init()
        buildMenu()
        supervisor.onChange = { [weak self] snapshot in self?.refresh(snapshot) }
    }

    func start() {
        refresh(supervisor.snapshot)
        supervisor.start()
    }

    func stop() {
        supervisor.stop()
    }

    // MARK: - 菜单

    private func buildMenu() {
        menu.autoenablesItems = false

        for item in [bridgeItem, deviceItem, batteryItem, codexItem,
                     bubbleItem, sessionItem, errorItem] {
            item.isEnabled = false
        }
        batteryItem.isHidden = true
        bubbleItem.isHidden = true
        sessionItem.isHidden = true
        errorItem.isHidden = true

        menu.addItem(bridgeItem)
        menu.addItem(deviceItem)
        menu.addItem(batteryItem)
        menu.addItem(codexItem)
        menu.addItem(bubbleItem)
        menu.addItem(sessionItem)
        menu.addItem(errorItem)
        menu.addItem(.separator())

        // 配网放在最前面: 设备连不上时这是用户第一个要找的东西。
        // 标题里不要加省略号: 这个名字会原样出现在设备屏幕上(那里的状态区只有
        // 200px 宽), 多一个字符就可能折行。
        let provisionItem = NSMenuItem(title: "配置 Wi-Fi",
                                       action: #selector(openProvisioning),
                                       keyEquivalent: "w")
        provisionItem.target = self
        menu.addItem(provisionItem)
        menu.addItem(.separator())

        // 手工推状态: 演示和联调时不用切回终端。
        let stateItem = NSMenuItem(title: "推送状态", action: nil, keyEquivalent: "")
        let stateMenu = NSMenu()
        for state in manualStates {
            let item = NSMenuItem(title: state.label,
                                  action: #selector(pushManualState(_:)),
                                  keyEquivalent: "")
            item.target = self
            item.representedObject = state.id
            stateMenu.addItem(item)
        }
        stateItem.submenu = stateMenu
        menu.addItem(stateItem)

        // 换宠物: 包由 Pet Bridge 从 ~/.codex/pets 现打现推, 设备不用重刷固件。
        petMenu.autoenablesItems = false
        petItem.submenu = petMenu
        menu.addItem(petItem)
        menu.addItem(.separator())

        let textItem = NSMenuItem(title: "发送气泡文字…",
                                  action: #selector(askForText), keyEquivalent: "t")
        textItem.target = self
        menu.addItem(textItem)
        menu.addItem(.separator())

        watchItem.title = "跟随 Codex 会话日志"
        watchItem.action = #selector(toggleWatching)
        watchItem.target = self
        menu.addItem(watchItem)

        let restartItem = NSMenuItem(title: "重启桥接",
                                     action: #selector(restartBridge), keyEquivalent: "r")
        restartItem.target = self
        menu.addItem(restartItem)

        let logItem = NSMenuItem(title: "打开日志",
                                 action: #selector(openLog), keyEquivalent: "l")
        logItem.target = self
        menu.addItem(logItem)

        let diagnosticsItem = NSMenuItem(title: "复制诊断信息",
                                         action: #selector(copyDiagnostics), keyEquivalent: "")
        diagnosticsItem.target = self
        menu.addItem(diagnosticsItem)
        menu.addItem(.separator())

        // 解释器路径**不放在这里**: 它是一个能改坏、改坏了又找不回原样的东西, 而它只在
        // "换宠物"这一步才有讲究。改成在「关于」里只读显示, 要改走命令行(见 README)。
        let aboutItem = NSMenuItem(title: "关于 Codex Pet Bridge",
                                   action: #selector(showAbout), keyEquivalent: "")
        aboutItem.target = self
        menu.addItem(aboutItem)
        menu.addItem(.separator())

        let quitItem = NSMenuItem(title: "退出 (同时停掉桥接)",
                                  action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        statusItem.menu = menu
        updateIcon(for: supervisor.snapshot)
    }

    // MARK: - 刷新显示

    private func refresh(_ snapshot: BridgeSnapshot) {
        updateIcon(for: snapshot)

        if let pid = snapshot.bridgePid {
            bridgeItem.title = "桥接：运行中 (pid \(pid))"
        } else if snapshot.lastError != nil {
            bridgeItem.title = "桥接：已退出"
        } else {
            bridgeItem.title = "桥接：启动中…"
        }

        if snapshot.deviceConnected {
            deviceItem.title = "设备：已连接 \(snapshot.devicePeer)"
        } else if snapshot.controlConnected {
            deviceItem.title = "设备：等待连接（确保设备与 Mac 在同一 2.4G 网络）"
        } else {
            deviceItem.title = "设备：控制通道未就绪"
        }

        // 电量只有设备主动上报过才有; 读不到(或还没收到)就把这一行藏掉,
        // 而不是显示一个可能过期的数字。
        if let battery = snapshot.batteryLabel {
            batteryItem.title = snapshot.batteryIsLow ? "⚠︎ 电量：\(battery)"
                                                      : "电量：\(battery)"
            batteryItem.isHidden = false
        } else {
            batteryItem.isHidden = true
        }

        var codex = "Codex：\(snapshot.codexStateLabel)"
        if !snapshot.stateSource.isEmpty {
            codex += "（\(snapshot.stateSource == "manual" ? "手工" : "日志")）"
        }
        codexItem.title = codex

        bubbleItem.title = "气泡：\(snapshot.bubbleText)"
        bubbleItem.isHidden = snapshot.bubbleText.isEmpty

        sessionItem.title = "跟随：\(snapshot.sessionName)"
        sessionItem.isHidden = snapshot.sessionName.isEmpty

        errorItem.title = "⚠︎ \(snapshot.lastError ?? "")"
        errorItem.isHidden = snapshot.lastError == nil

        watchItem.state = snapshot.watchingCodex ? .on : .off

        refreshPetMenu(snapshot)
    }

    // MARK: - 宠物子菜单

    /// 子菜单内容是活的(宠物列表 + 传输进度), 所以按需重建。与顶层菜单不同, 这一份
    /// 平时是合上的, 重建不会让高亮乱跳。
    private func refreshPetMenu(_ snapshot: BridgeSnapshot) {
        // 顶栏标题带一眼就能看见的信息: 传输中报进度, 否则报设备上当前那只。
        if let short = snapshot.petTransfer.shortLabel {
            petItem.title = "宠物：\(short)"
        } else if snapshot.petPackage.isEmpty {
            petItem.title = "宠物"
        } else {
            petItem.title = "宠物：\(snapshot.petPackage)"
        }

        // 内容指纹。传输期间 petxfer 每 16 KiB 就报一次进度, 没必要每次都把
        // NSMenuItem 全部重造。
        let key = [
            snapshot.petPackage,
            snapshot.petsAvailable ? "1" : "0",
            snapshot.petBusy ? "1" : "0",
            snapshot.deviceConnected ? "1" : "0",
            snapshot.petsDir,
            snapshot.petPackAvailable.map { $0 ? "1" : "0" } ?? "-",
            snapshot.petPackHint ?? "",
            snapshot.pets.map { "\($0.id)/\($0.size)/\($0.problem ?? "")" }
                .joined(separator: ","),
            snapshot.petTransfer.state, snapshot.petTransfer.id,
            String(snapshot.petTransfer.sent), String(snapshot.petTransfer.total),
            snapshot.petTransfer.message ?? "",
        ].joined(separator: "|")
        guard key != petMenuKey else { return }
        petMenuKey = key

        petMenu.removeAllItems()

        // 进度/结局那一行在最上面: 用户刚点完菜单, 这就是他要看的反馈。
        if let label = snapshot.petTransfer.label {
            petMenu.addItem(disabledPetItem(label))
            petMenu.addItem(.separator())
        }

        // 打包是桥接侧那台 python 干的活。缺 Pillow 时**没缓存过的**那几只点了必然
        // 失败, 所以先在顶上说清楚缺什么、去哪儿看, 而不是让用户点一只消失一只。
        let unpackable = snapshot.petPackAvailable == false
            && snapshot.pets.contains { !$0.cached && $0.problem == nil }
        if unpackable {
            petMenu.addItem(disabledPetItem("⚠︎ 无法打包：\(snapshot.petPackHint ?? "桥接的 python 没有 Pillow")"))
            petMenu.addItem(disabledPetItem("   在「关于」里能看到是哪个解释器, 换上带 Pillow 的见 README"))
            petMenu.addItem(.separator())
        }

        if !snapshot.petsAvailable {
            petMenu.addItem(disabledPetItem("宠物库未启用（桥接启动时带了 --no-pets）"))
        } else if snapshot.pets.isEmpty {
            petMenu.addItem(disabledPetItem("~/.codex/pets 里没有宠物"))
        } else {
            for entry in snapshot.pets {
                petMenu.addItem(petMenuItem(entry, snapshot))
            }
        }

        petMenu.addItem(.separator())

        let refreshItem = NSMenuItem(title: "刷新列表（刚在 Codex 里下了新宠物）",
                                     action: #selector(refreshPets), keyEquivalent: "")
        refreshItem.target = self
        petMenu.addItem(refreshItem)

        let openItem = NSMenuItem(title: "打开宠物目录", action: #selector(openPetsDir),
                                  keyEquivalent: "")
        openItem.target = self
        openItem.representedObject = snapshot.petsDir
        openItem.isEnabled = !snapshot.petsDir.isEmpty
        petMenu.addItem(openItem)
    }

    private func disabledPetItem(_ title: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }

    private func petMenuItem(_ entry: PetEntry, _ snapshot: BridgeSnapshot) -> NSMenuItem {
        let installed = entry.id == snapshot.petPackage
        var title = entry.display.isEmpty ? entry.id : "\(entry.display)（\(entry.id)）"
        if entry.size > 0 {
            title += String(format: " · %.1f MiB", Double(entry.size) / 1_048_576)
        }

        let item = NSMenuItem(title: title, action: #selector(installPet(_:)),
                              keyEquivalent: "")
        item.target = self
        item.representedObject = entry.id
        item.state = installed ? .on : .off

        if let problem = entry.problem {
            item.title = "⚠︎ \(title)：\(problem)"
            item.isEnabled = false
        } else if !entry.cached && snapshot.petPackAvailable == false {
            // 没缓存过 = 得现打, 而这边打不出来。禁掉比让用户点了再看报错更省事。
            item.title = "⧗ \(title)：缺 Pillow, 打不出包"
            item.isEnabled = false
        } else if installed {
            item.isEnabled = false            // 已经是它了
        } else if !snapshot.deviceConnected {
            item.isEnabled = false            // 设备没连上, 桥接只会回一句"设备还没连上"
        } else {
            item.isEnabled = !snapshot.petBusy
        }
        return item
    }

    private func updateIcon(for snapshot: BridgeSnapshot) {
        let name: String
        if !snapshot.deviceConnected {
            name = "pawprint"
        } else {
            switch snapshot.codexState {
            case "working": name = "gearshape.2.fill"
            case "waiting": name = "exclamationmark.bubble.fill"
            case "ready": name = "checkmark.circle.fill"
            case "failed": name = "xmark.octagon.fill"
            default: name = "pawprint.fill"
            }
        }

        if let image = NSImage(systemSymbolName: name,
                               accessibilityDescription: "Codex 宠物") {
            image.isTemplate = true
            statusItem.button?.image = image
            statusItem.button?.title = ""
        } else {
            statusItem.button?.image = nil
            statusItem.button?.title = "🐾"
        }
        // 图标淡显表示"设备不在线", 这是 AppKit 表达不可用状态的标准做法。
        statusItem.button?.appearsDisabled = !snapshot.deviceConnected
        statusItem.button?.toolTip = tooltip(for: snapshot)
    }

    private func tooltip(for snapshot: BridgeSnapshot) -> String {
        if !snapshot.deviceConnected { return "Codex 宠物：设备未连接" }
        var text = "Codex 宠物：\(snapshot.codexStateLabel)"
        if let battery = snapshot.batteryLabel {
            text += "\n电量：\(battery)"
        }
        if !snapshot.bubbleText.isEmpty { text += "\n\(snapshot.bubbleText)" }
        return text
    }

    // MARK: - 动作

    @objc private func pushManualState(_ sender: NSMenuItem) {
        guard let state = sender.representedObject as? String else { return }
        supervisor.pushState(state)
    }

    @objc private func openProvisioning() {
        provisionController.show()
    }

    @objc private func installPet(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String else { return }
        NSApp.activate(ignoringOtherApps: true)

        // 确认一次是值得的: 传输期间设备的槽是先清空再写的, 中途断电/断网会留下
        // "还没有宠物"的设备, 得再点一次。这不是一个可以随手误触的操作。
        let alert = NSAlert()
        alert.messageText = "把设备上的宠物换成 \(id)？"
        alert.informativeText = """
            桥接会把这只重新打包, 经 Wi-Fi 推到设备上(通常几十秒, 期间设备屏幕上 \
            会显示接收进度)。

            注意: 设备是先把槽清空再写入的 —— 传输中途断电或断网的话, 设备上就没有 \
            宠物了, 重新点一次即可。
            """
        alert.addButton(withTitle: "开始传输")
        alert.addButton(withTitle: "取消")
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        supervisor.installPet(id)
    }

    @objc private func refreshPets() {
        supervisor.refreshPets()
    }

    @objc private func openPetsDir(_ sender: NSMenuItem) {
        guard let path = sender.representedObject as? String, !path.isEmpty else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }

    @objc private func askForText() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "发送气泡文字"
        alert.informativeText = "只改设备上的文字, 不改变当前状态。Return 换行, 点「发送」下发(设备文字区显示两行)。"
        alert.addButton(withTitle: "发送")
        alert.addButton(withTitle: "取消")

        // 多行输入: 单行 NSTextField 换成 NSTextView 套滚动视图, Return 换行、
        // 发送靠按钮。Cmd+V 等快捷键由 AppDelegate 装的编辑菜单兜底。
        let textView = NSTextView(frame: NSRect(x: 0, y: 0, width: 280, height: 64))
        textView.font = NSFont.systemFont(ofSize: NSFont.systemFontSize)
        textView.isRichText = false
        textView.usesFindBar = true
        let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: 280, height: 64))
        scroll.borderType = .bezelBorder
        scroll.hasVerticalScroller = true
        scroll.documentView = textView
        alert.accessoryView = scroll
        alert.window.initialFirstResponder = textView

        if alert.runModal() == .alertFirstButtonReturn {
            let text = textView.string.trimmingCharacters(in: .whitespacesAndNewlines)
            if !text.isEmpty { supervisor.sendText(text) }
        }
    }

    @objc private func toggleWatching() {
        supervisor.setWatchingCodex(!supervisor.snapshot.watchingCodex)
    }

    @objc private func restartBridge() {
        supervisor.restart()
    }

    @objc private func openLog() {
        BridgePaths.ensureSupportDir()
        if !FileManager.default.fileExists(atPath: BridgePaths.logPath) {
            FileManager.default.createFile(atPath: BridgePaths.logPath, contents: nil)
        }
        NSWorkspace.shared.open(URL(fileURLWithPath: BridgePaths.logPath))
    }

    @objc private func copyDiagnostics() {
        let settings = BridgeSettings.load()
        let snapshot = supervisor.snapshot
        var lines = [
            "Codex Pet Bridge \(BuildConfig.version)",
            "桥接脚本: \(settings.scriptPath ?? "未找到（包不完整）")",
            "python: \(settings.pythonPath)（\(settings.pythonSourceLabel)）",
            "打包: \(snapshot.petPackAvailable.map { $0 ? "可用" : "不可用（缺 Pillow）" } ?? "未知")",
            "端口: \(settings.port)",
            "控制通道: \(BridgePaths.socketPath)",
            "桥接: \(snapshot.bridgePid.map { "运行中 pid \($0)" } ?? "未运行")",
            "控制通道已连: \(snapshot.controlConnected)",
            "设备: \(snapshot.deviceConnected ? snapshot.devicePeer : "未连接")",
            "Codex 状态: \(snapshot.codexState) (\(snapshot.stateSource))",
            "气泡: \(snapshot.bubbleText)",
            "跟随会话: \(snapshot.sessionName.isEmpty ? "-" : snapshot.sessionName)",
            "固件: \(snapshot.firmware.isEmpty ? "-" : snapshot.firmware)",
            "宠物包: \(snapshot.petPackage.isEmpty ? "-" : snapshot.petPackage)",
            "电量: \(snapshot.batteryLabel ?? "-")",
        ]
        if let error = snapshot.lastError { lines.append("最后错误: \(error)") }
        let tail = supervisor.logTail(lines: 15)
        if !tail.isEmpty {
            lines.append("--- 日志尾部 ---")
            lines.append(tail)
        }

        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(lines.joined(separator: "\n"), forType: .string)
    }

    /// 「关于」是**唯一**能看到解释器路径的地方 —— 它只读, 不会被误改。
    ///
    /// 解释器决定"换宠物"能不能把图集打成 `.pet`(要 Pillow), 所以路径、来源(自动探测还是
    /// 命令行指定)、有无 Pillow 都在这里交代清楚: 出问题时要能一眼看全, 而不是去猜
    /// "当初改的是哪一个"。菜单里那个能改路径的入口已经拿掉了。
    @objc private func showAbout() {
        let settings = BridgeSettings.load()
        let hasPillow = settings.pythonHasPillow
        let pillow = hasPillow ? "有 Pillow" : "⚠︎ 没有 Pillow"

        var lines = [
            "把 Mac 上的 Codex 状态推给 AI Passport 上的宠物。",
            "",
            "python 解释器：\(settings.pythonPath)（\(settings.pythonSourceLabel) · \(pillow)）",
        ]
        if !hasPillow {
            lines.append("    换宠物要把图集打成 .pet, 这一步需要 Pillow。可以这样装：")
            lines.append("    \(settings.pythonPath) -m pip install Pillow")
        }
        lines += [
            "桥接脚本：\(settings.scriptPath ?? "未找到（包不完整）")",
            "桥接端口：\(settings.port)",
            "控制通道：\(BridgePaths.socketPath)",
            "",
            "设备是 TCP 客户端, 会主动连到这台 Mac, 所以设备侧只需要填对 IP 和端口。",
        ]

        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Codex Pet Bridge \(BuildConfig.version)"
        alert.informativeText = lines.joined(separator: "\n")
        alert.runModal()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }
}
