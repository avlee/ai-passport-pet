// tools/menubar/Sources/ProvisionWindow.swift
//
// 「配置 Wi-Fi」窗口。
//
// 界面只负责收集三样东西: 配对码(设备屏幕上显示的那 4 位)、网络名称、密码。
// 配对端地址默认填本机的局域网地址 —— 设备要主动连过来, 而用户通常不知道自己的
// IP 是多少, 这一项由应用代填能省掉一类最常见的配网失败。
//
// 所有 BLE 交互都在 ProvisionClient 里, 这个文件不做任何 CoreBluetooth 的事。

import AppKit

/// 本机网络信息。都通过系统工具/内核查询, 不引入第三方库。
enum NetworkInfo {
    /// 本机的局域网地址。用一个不会真的发出去的 UDP connect 问内核要出口地址 ——
    /// 与 tools/pet_bridge.py 里的做法一致, 不需要遍历网卡。
    static func localIPv4() -> String? {
        let socketFD = socket(AF_INET, SOCK_DGRAM, 0)
        guard socketFD >= 0 else { return nil }
        defer { close(socketFD) }

        var address = sockaddr_in()
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = in_port_t(1).bigEndian
        address.sin_addr.s_addr = inet_addr("192.168.1.1")

        let result = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(socketFD, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard result == 0 else { return nil }

        var local = sockaddr_in()
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let ok = withUnsafeMutablePointer(to: &local) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(socketFD, $0, &length)
            }
        }
        guard ok == 0 else { return nil }

        var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        var raw = local.sin_addr
        guard inet_ntop(AF_INET, &raw, &buffer, socklen_t(INET_ADDRSTRLEN)) != nil else {
            return nil
        }
        let text = String(cString: buffer)
        return text == "0.0.0.0" ? nil : text
    }

    /// 当前连着的 Wi-Fi 名字。走 networksetup, 走的不是 Wi-Fi 或者没权限时会失败,
    /// 那就让用户自己填。
    static func currentSSID() -> String? {
        for interface in ["en0", "en1"] {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/sbin/networksetup")
            process.arguments = ["-getairportnetwork", interface]
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = Pipe()

            guard (try? process.run()) != nil else { continue }
            process.waitUntilExit()

            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            guard let output = String(data: data, encoding: .utf8) else { continue }
            guard let range = output.range(of: ":") else { continue }
            let name = output[range.upperBound...].trimmingCharacters(in: .whitespacesAndNewlines)
            if !name.isEmpty, !name.contains("not associated") { return name }
        }
        return nil
    }
}

final class ProvisionWindowController: NSObject, NSWindowDelegate {
    private var window: NSWindow?
    private var client: ProvisionClient?

    private let settings = BridgeSettings.load()

    private let devicePopup = NSPopUpButton()
    private let pinField = NSTextField()
    private let ssidField = NSTextField()
    private let passwordField = NSSecureTextField()
    private let hostField = NSTextField()
    private let portField = NSTextField()

    private let scanButton = NSButton()
    private let startButton = NSButton()
    private let forgetButton = NSButton()
    private let logView = NSTextView()

    private var connected = false
    private var paired = false

    func show() {
        NSApp.activate(ignoringOtherApps: true)
        if window == nil {
            window = buildWindow()
        }
        window?.makeKeyAndOrderFront(nil)
        beginScan()
    }

    // MARK: - 窗口

    private func buildWindow() -> NSWindow {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 540),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered, defer: false)
        window.title = "配置 Wi-Fi — Codex 宠物"
        window.delegate = self
        window.center()

        devicePopup.removeAllItems()
        devicePopup.addItem(withTitle: "扫描中…")
        devicePopup.target = self
        devicePopup.action = #selector(deviceChanged)

        pinField.placeholderString = "设备屏幕上显示的 4 位数字"
        pinField.maximumNumberOfLines = 1

        ssidField.placeholderString = "2.4G 网络名称"
        ssidField.stringValue = NetworkInfo.currentSSID() ?? ""

        passwordField.placeholderString = "Wi-Fi 密码"

        hostField.placeholderString = "本机地址"
        hostField.stringValue = NetworkInfo.localIPv4() ?? ""

        portField.stringValue = String(settings.port)
        portField.placeholderString = "8765"

        let wifiButton = NSButton(title: "用当前 Wi-Fi", target: self,
                                  action: #selector(useCurrentSSID))
        wifiButton.bezelStyle = .rounded

        let grid = NSGridView(views: [
            [label("设备"), devicePopup, scanButtonTitled()],
            [label("配对码"), pinField, NSGridCell.emptyContentView],
            [label("网络名称"), ssidField, wifiButton],
            [label("密码"), passwordField, NSGridCell.emptyContentView],
            [label("配对端"), hostField, portRow()],
        ])
        grid.rowSpacing = 10
        grid.columnSpacing = 10
        grid.column(at: 0).xPlacement = .trailing
        grid.column(at: 1).width = 280
        grid.translatesAutoresizingMaskIntoConstraints = false

        let hint = NSTextField(labelWithString:
            "配对码显示在宠物屏幕上(长按下键可以打开配网界面)。")
        hint.textColor = .secondaryLabelColor
        hint.font = .systemFont(ofSize: 11)

        startButton.title = "开始配网"
        startButton.target = self
        startButton.action = #selector(startProvision)
        startButton.bezelStyle = .rounded
        startButton.keyEquivalent = "\r"

        forgetButton.title = "清空设备参数"
        forgetButton.target = self
        forgetButton.action = #selector(forget)
        forgetButton.bezelStyle = .rounded
        forgetButton.isEnabled = false

        let closeButton = NSButton(title: "关闭", target: self,
                                   action: #selector(closeWindow))
        closeButton.bezelStyle = .rounded
        closeButton.keyEquivalent = "\u{1b}"

        let buttons = NSStackView(views: [startButton, forgetButton,
                                          NSView(), closeButton])
        buttons.orientation = .horizontal
        buttons.spacing = 10
        buttons.translatesAutoresizingMaskIntoConstraints = false

        logView.isEditable = false
        logView.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        logView.textContainerInset = NSSize(width: 6, height: 6)
        let scroll = NSScrollView()
        scroll.documentView = logView
        scroll.hasVerticalScroller = true
        scroll.borderType = .bezelBorder
        scroll.translatesAutoresizingMaskIntoConstraints = false

        let content = NSView()
        content.addSubview(grid)
        content.addSubview(hint)
        content.addSubview(buttons)
        content.addSubview(scroll)
        hint.translatesAutoresizingMaskIntoConstraints = false

        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: content.topAnchor, constant: 18),
            grid.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 18),
            grid.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -18),

            hint.topAnchor.constraint(equalTo: grid.bottomAnchor, constant: 12),
            hint.leadingAnchor.constraint(equalTo: grid.leadingAnchor),

            buttons.topAnchor.constraint(equalTo: hint.bottomAnchor, constant: 12),
            buttons.leadingAnchor.constraint(equalTo: grid.leadingAnchor),
            buttons.trailingAnchor.constraint(equalTo: grid.trailingAnchor),

            scroll.topAnchor.constraint(equalTo: buttons.bottomAnchor, constant: 12),
            scroll.leadingAnchor.constraint(equalTo: grid.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: grid.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -18),
            scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 160),
        ])

        window.contentView = content
        setControlsEnabled(connected: false, paired: false)
        return window
    }

    private func label(_ text: String) -> NSTextField {
        NSTextField(labelWithString: text)
    }

    private func scanButtonTitled() -> NSButton {
        scanButton.title = "重新扫描"
        scanButton.target = self
        scanButton.action = #selector(beginScan)
        scanButton.bezelStyle = .rounded
        return scanButton
    }

    private func portRow() -> NSView {
        let prefix = NSTextField(labelWithString: "端口")
        prefix.textColor = .secondaryLabelColor
        portField.widthAnchor.constraint(equalToConstant: 70).isActive = true
        let stack = NSStackView(views: [portField, prefix])
        stack.orientation = .horizontal
        stack.spacing = 6
        return stack
    }

    private func setControlsEnabled(connected: Bool, paired: Bool) {
        self.connected = connected
        self.paired = paired
        startButton.isEnabled = connected && !paired
        forgetButton.isEnabled = paired
        scanButton.isEnabled = !connected
        devicePopup.isEnabled = !connected
    }

    private func append(_ text: String) {
        DispatchQueue.main.async {
            let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .none,
                                                      timeStyle: .medium)
            self.logView.string += "[\(stamp)] \(text)\n"
            self.logView.scrollToEndOfDocument(nil)
        }
    }

    // MARK: - 动作

    @objc private func beginScan() {
        client?.stop()
        paired = false
        setControlsEnabled(connected: false, paired: false)
        devicePopup.removeAllItems()
        devicePopup.addItem(withTitle: "扫描中…")
        append("开始扫描…")

        let client = ProvisionClient { [weak self] event in
            self?.handle(event)
        }
        self.client = client
        client.start()
    }

    @objc private func deviceChanged() {
        // 目前只连信号最强的那台, 下拉框只是让用户看到扫到了什么。
    }

    @objc private func useCurrentSSID() {
        if let ssid = NetworkInfo.currentSSID() {
            ssidField.stringValue = ssid
            append("已填入当前 Wi-Fi: \(ssid)")
        } else {
            append("读不到当前 Wi-Fi 名字, 请手动填写")
        }
    }

    @objc private func startProvision() {
        let pin = pinField.stringValue.trimmingCharacters(in: .whitespaces)
        guard pin.count == 4, pin.allSatisfy({ $0.isNumber }) else {
            append("⚠︎ 配对码要填 4 位数字(看设备屏幕)")
            return
        }
        guard !ssidField.stringValue.trimmingCharacters(in: .whitespaces).isEmpty else {
            append("⚠︎ 网络名称不能为空")
            return
        }
        guard let port = Int(portField.stringValue), port > 0, port < 65536 else {
            append("⚠︎ 端口不合法")
            return
        }
        append("把配对码发给设备…")
        startButton.isEnabled = false
        client?.pair(pin: pin)
    }

    @objc private func forget() {
        append("请设备清空已保存的网络参数…")
        client?.forget()
    }

    @objc private func closeWindow() {
        window?.orderOut(nil)
        client?.stop()
        client = nil
    }

    func windowWillClose(_ notification: Notification) {
        client?.stop()
        client = nil
    }

    // MARK: - 事件

    private func handle(_ event: ProvisionEvent) {
        switch event {
        case .log(let text):
            append(text)

        case .scanning:
            break

        case .found(let devices):
            devicePopup.removeAllItems()
            if devices.isEmpty {
                devicePopup.addItem(withTitle: "没有扫到设备")
                append("没有扫到配网中的设备。确认宠物屏幕停在配对码界面。")
            } else {
                for device in devices {
                    devicePopup.addItem(withTitle: "\(device.name)  \(device.rssi) dBm")
                }
                append("扫到 \(devices.count) 台, 连接信号最强的: \(devices[0].name)")
            }

        case .connecting(let device):
            append("正在连接 \(device.name)…")

        case .connected(let info):
            setControlsEnabled(connected: true, paired: false)
            if let device = info["device"] as? String {
                let fw = info["fw"] as? String ?? "?"
                let pet = info["pet"] as? String ?? "?"
                append("设备就绪: \(device), 固件 \(fw), 宠物包 \(pet)")
            } else {
                append("设备就绪, 请输入屏幕上的配对码")
            }
            if let ssid = info["ssid"] as? String, !ssid.isEmpty,
               ssidField.stringValue.isEmpty {
                ssidField.stringValue = ssid
            }
            window?.makeFirstResponder(pinField)

        case .status(let status):
            append(status.describe)
            switch status {
            case .paired:
                setControlsEnabled(connected: true, paired: true)
                // 配对一通过就自动下发参数: 用户的意图本来就是"把这台设备接上
                // 这个 Wi-Fi", 再让他点一次只是多余的步骤。
                guard let port = Int(portField.stringValue) else { return }
                append("配对成功, 发送 Wi-Fi 参数…")
                client?.apply(ssid: ssidField.stringValue.trimmingCharacters(in: .whitespaces),
                              password: passwordField.stringValue,
                              host: hostField.stringValue.trimmingCharacters(in: .whitespaces),
                              port: port)
            case .pinRejected:
                startButton.isEnabled = true
                window?.makeFirstResponder(pinField)
            case .done:
                append("设备会自己连上 Wi-Fi, 随后连到这台 Mac 的桥接端口。")
            case .deviceFailed, .deviceError:
                startButton.isEnabled = true
            default:
                break
            }

        case .disconnected(let reason):
            // 先记下状态: setControlsEnabled 会把 paired 清掉, 而下面的提示要看它。
            let hadPaired = paired
            setControlsEnabled(connected: false, paired: false)
            append("连接已断开: \(reason)")
            if hadPaired {
                append("如果设备屏幕已提示配网完成, 这是正常的 —— 设备省电会主动关掉蓝牙。")
            }

        case .failed(let message):
            setControlsEnabled(connected: false, paired: false)
            append("⚠︎ \(message)")
        }
    }
}
