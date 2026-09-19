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
    private let repoItem = NSMenuItem()
    private let watchItem = NSMenuItem()

    private let manualStates: [(id: String, label: String)] = [
        ("idle", "空闲"),
        ("working", "工作中"),
        ("waiting", "等待确认"),
        ("ready", "已完成"),
        ("failed", "出错了"),
    ]

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
                     bubbleItem, sessionItem, errorItem, repoItem] {
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

        repoItem.action = #selector(chooseRepo)
        repoItem.target = self
        repoItem.isEnabled = true
        menu.addItem(repoItem)

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
        repoItem.title = "项目目录：\(BridgeSettings.load().repoPath)"
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

    @objc private func askForText() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "发送气泡文字"
        alert.informativeText = "只改设备上的一句话, 不改变当前状态。"
        alert.addButton(withTitle: "发送")
        alert.addButton(withTitle: "取消")

        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        field.placeholderString = "例如：正在重构登录模块"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field

        if alert.runModal() == .alertFirstButtonReturn {
            let text = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
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
            "项目目录: \(settings.repoPath)",
            "python: \(settings.pythonPath)",
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

    @objc private func chooseRepo() {
        NSApp.activate(ignoringOtherApps: true)
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.message = "选择 ai-passport 项目目录（里面应有 tools/pet_bridge.py）"
        panel.directoryURL = URL(fileURLWithPath: BridgeSettings.load().repoPath)

        guard panel.runModal() == .OK, let url = panel.url else { return }
        let script = url.appendingPathComponent("tools/pet_bridge.py")
        guard FileManager.default.isReadableFile(atPath: script.path) else {
            let alert = NSAlert()
            alert.messageText = "这个目录里没有 tools/pet_bridge.py"
            alert.informativeText = "请选择 ai-passport 项目根目录。"
            alert.runModal()
            return
        }
        UserDefaults.standard.set(url.path, forKey: PrefKey.repoPath)
        supervisor.restart()
    }

    @objc private func showAbout() {
        let settings = BridgeSettings.load()
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Codex Pet Bridge \(BuildConfig.version)"
        alert.informativeText = """
            把 Mac 上的 Codex 状态推给 AI Passport 上的宠物。

            项目目录：\(settings.repoPath)
            桥接端口：\(settings.port)
            控制通道：\(BridgePaths.socketPath)

            设备是 TCP 客户端, 会主动连到这台 Mac, 所以设备侧只需要填对 IP 和端口。
            """
        alert.runModal()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }
}
