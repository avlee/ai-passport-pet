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
    /// 本机的局域网地址。
    ///
    /// 早期实现是「UDP connect 到 192.168.1.1 问内核要出口地址」(与 pet_bridge.py
    /// 一致): 简单, 但有两个坑 —— 装 VPN 时 192.168.1.1 从虚拟网卡走, 拿回来的是
    /// 设备根本连不通的 VPN 地址; 没有 IPv4 默认路由时 connect 直接失败。改成枚举
    /// 网卡: 只在 UP 且 RUNNING 的实体网卡里选, 优先 en0(Wi-Fi/网线), 跳过
    /// utun/awdl 等虚拟接口和 169.254 链路本地地址 —— 设备要连的是 Mac 所在的
    /// 局域网, 只有从实体网卡上取才是对的。
    static func localIPv4() -> String? {
        var candidates: [(name: String, ip: String)] = []

        var ifaddrPtr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddrPtr) == 0, let first = ifaddrPtr else { return nil }
        defer { freeifaddrs(first) }

        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            cursor = entry.pointee.ifa_next

            guard let addr = entry.pointee.ifa_addr,
                  addr.pointee.sa_family == UInt8(AF_INET) else { continue }

            let flags = Int32(entry.pointee.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_RUNNING != 0,
                  flags & IFF_LOOPBACK == 0 else { continue }

            let name = String(cString: entry.pointee.ifa_name)
            // 这些都是虚拟/辅助接口: VPN(utun/tap/tun/gif/stf)、AirDrop 与侧信道
            // (awdl/llw)、热点共享(ap/bridge)、虚拟机(docker/VMware 的 bridge 与
            // vmnet 系列)、Apple 网络处理器(anpi)。设备永远不需要连这些地址。
            let virtual = ["utun", "awdl", "llw", "bridge", "ipsec", "stf",
                           "gif", "tap", "tun", "vlan", "vmnet", "vmenet",
                           "anpi", "ap"]
            if virtual.contains(where: name.hasPrefix) { continue }

            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            // NI_NUMERICHOST: 只做数字转换, 不发起任何 DNS 查询。
            guard getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host,
                              socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0
            else { continue }

            let ip = String(cString: host)
            // 169.254.x.x 是没拿到 DHCP 时的自分配地址, 拿它配网必然失败。
            if ip.isEmpty || ip.hasPrefix("169.254.") { continue }
            candidates.append((name, ip))
        }

        guard !candidates.isEmpty else { return nil }

        func rank(_ name: String) -> Int {
            if name == "en0" { return 0 }
            if name.hasPrefix("en") { return 1 }
            return 2
        }
        // 排序保证结果确定: 同为 en 按名字排(en0 在 en1 前), 不依赖枚举顺序。
        let best = candidates.min { a, b in
            let (ra, rb) = (rank(a.name), rank(b.name))
            return ra != rb ? ra < rb : a.name < b.name
        }
        return best?.ip
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

/// 记住配网成功过的网络(ssid + 密码), 存在支持目录下 0600 的 JSON 里。
///
/// 为什么不是 Keychain: 这个应用是 ad-hoc 签名, 每次重新构建 cdhash 都会变, 而
/// Keychain 的访问控制锚在签名上 —— 换一次构建就反复弹授权甚至读不回来。Wi-Fi 密码
/// 放进当前用户家目录里仅本人可读的文件, 是"不折腾用户"和"不裸奔"之间务实的上限。
enum SavedWifi {
    struct Entry: Codable, Equatable {
        var ssid: String
        var password: String
    }

    static var fileURL: URL {
        BridgePaths.supportDir.appendingPathComponent("saved-wifi.json")
    }

    static func load() -> [Entry] {
        guard let data = try? Data(contentsOf: fileURL),
              let entries = try? JSONDecoder().decode([Entry].self, from: data)
        else { return [] }
        return entries
    }

    /// 最新在前; 同名网络覆盖旧密码; 上限 8 条, 超了淘汰最旧的。
    static func remember(ssid: String, password: String) {
        guard !ssid.isEmpty else { return }
        BridgePaths.ensureSupportDir()
        var entries = load().filter { $0.ssid != ssid }
        entries.insert(Entry(ssid: ssid, password: password), at: 0)
        if entries.count > 8 {
            entries.removeLast(entries.count - 8)
        }
        guard let data = try? JSONEncoder().encode(entries) else { return }
        FileManager.default.createFile(atPath: fileURL.path, contents: data,
                                       attributes: [.posixPermissions: 0o600])
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
    private let savedPopup = NSPopUpButton()
    private var savedRow: NSGridRow?

    private var connected = false
    private var paired = false

    func show() {
        NSApp.activate(ignoringOtherApps: true)
        if window == nil {
            window = buildWindow()
        }
        // 浮在普通窗口层之上, 原因看 buildWindow 里的注释 —— 蓝牙授权弹窗收掉之后
        // macOS 会把焦点还给之前的前台应用, 靠抢焦点修不了, 只能让窗口自己站得住。
        window?.level = .floating
        window?.makeKeyAndOrderFront(nil)
        // 窗口是复用的, buildWindow 里那次预填只在第一次生效: 之后换了网络或
        // DHCP 换了地址, 旧 IP 会一直留在框里发给设备, 设备自然连不上。所以
        // 每次露面都重新取一次(见 refreshHostField 的注释)。
        refreshHostField()
        refreshSavedNetworks()
        beginScan()
    }

    /// 重新探测本机地址并填进「配对端」。取得到就覆盖 —— 手动改这一栏的场景只
    /// 应该发生在自动探测失败时, 覆盖是安全的; 取不到就保留现有内容, 让用户
    /// 自己填, 并在日志里说清楚为什么框是空的。
    private func refreshHostField() {
        if let ip = NetworkInfo.localIPv4() {
            hostField.stringValue = ip
        } else if hostField.stringValue.isEmpty {
            append("⚠︎ 没能自动取到本机局域网地址, 请手动填写「配对端」一栏")
        }
    }

    // MARK: - 窗口

    private func buildWindow() -> NSWindow {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 480, height: 540),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered, defer: false)
        // NSWindow 程序化创建时 isReleasedWhenClosed 默认是 true: 点红关闭钮 AppKit
        // 会自己 release 一次, ARC 再 release 就是过度释放 —— 窗口死了而 `window` 属性
        // 还悬着, 下一次点「配置 Wi-Fi」在 makeKeyAndOrderFront 上 SIGSEGV(2026-09-20
        // 五份崩溃报告同一个栈)。设为 false 后生命周期完全交给 ARC, 关掉再开是复用。
        window.isReleasedWhenClosed = false
        window.title = "配置 Wi-Fi — Codex 宠物"
        window.delegate = self
        window.center()
        // 蓝牙授权弹窗(TCC)归系统进程所有: 它弹出时抢走焦点, 用户点完"允许"之后
        // macOS 把前台还给弹窗之前的应用(不是我们这个 .accessory 应用), 配网页就被
        // 盖住了。修法不是抢焦点回来 —— 那会跟用户主动切走窗口的行为打架 —— 而是让
        // 这个窗口浮在普通窗口层(show() 里设 .floating), 直到配完关掉; 顺带允许它
        // 出现在全屏空间之上, 不然对着全屏 IDE 配网时什么都看不见。
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]

        devicePopup.removeAllItems()
        devicePopup.addItem(withTitle: "扫描中…")
        devicePopup.target = self
        devicePopup.action = #selector(deviceChanged)

        pinField.placeholderString = "设备屏幕上显示的 4 位数字"
        pinField.maximumNumberOfLines = 1

        ssidField.placeholderString = "2.4G 网络名称"

        passwordField.placeholderString = "Wi-Fi 密码"

        hostField.placeholderString = "本机地址"
        hostField.stringValue = NetworkInfo.localIPv4() ?? ""

        portField.stringValue = String(settings.port)
        portField.placeholderString = "8765"

        // 记住配网成功过的网络: SSID 是定位类敏感数据, macOS 不会把真 SSID 给没有
        // 定位权限的进程(实测 networksetup 只会谎报"未关联"), 所以"自动读当前 Wi-Fi"
        // 那条路在这台系统上走不通, 干脆拿掉; 已保存列表是零权限又能真正省输入的替代。
        savedPopup.target = self
        savedPopup.action = #selector(savedNetworkChosen)
        let savedLabel = label("已保存")
        savedLabel.textColor = .secondaryLabelColor

        let grid = NSGridView(views: [
            [label("设备"), devicePopup, scanButtonTitled()],
            [label("配对码"), pinField, NSGridCell.emptyContentView],
            [savedLabel, savedPopup, NSGridCell.emptyContentView],
            [label("网络名称"), ssidField, NSGridCell.emptyContentView],
            [label("密码"), passwordField, NSGridCell.emptyContentView],
            [label("配对端"), hostField, portRow()],
        ])
        // "已保存"在第 2 行: 没有记住过任何网络时整行藏掉, 界面跟原来一样。
        savedRow = grid.row(at: 2)
        savedRow?.isHidden = true
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

        // maxScanRounds nil = 扫不到就一直扫: 设备进配网页可以发生在开窗之后,
        // 一次扫空就报错对"先开窗、后长按下键"的顺序是误伤。
        let client = ProvisionClient(maxScanRounds: nil) { [weak self] event in
            self?.handle(event)
        }
        self.client = client
        client.start()
    }

    @objc private func deviceChanged() {
        // 目前只连信号最强的那台, 下拉框只是让用户看到扫到了什么。
    }

    /// 窗口可能被复用(修 use-after-free 之后关掉再开是同一个窗口), 所以每次露面都
    /// 重读一遍已保存列表。
    private func refreshSavedNetworks() {
        let entries = SavedWifi.load()
        savedPopup.removeAllItems()
        guard !entries.isEmpty else {
            savedRow?.isHidden = true
            return
        }
        for entry in entries { savedPopup.addItem(withTitle: entry.ssid) }
        savedRow?.isHidden = false
    }

    @objc private func savedNetworkChosen() {
        let entries = SavedWifi.load()
        let index = savedPopup.indexOfSelectedItem
        guard index >= 0, index < entries.count else { return }
        let entry = entries[index]
        ssidField.stringValue = entry.ssid
        passwordField.stringValue = entry.password
        append("已填入记住的网络: \(entry.ssid)")
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
        // 配对成功后会自动把「配对端」一起下发, 与其让设备拿着空地址白等几秒
        // 再报 connect failed, 不如在这里就拦下来把话说清楚。
        guard !hostField.stringValue.trimmingCharacters(in: .whitespaces).isEmpty else {
            append("⚠︎ 配对端地址不能为空(应为这台 Mac 的局域网 IP)")
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
                // 不再 append 一行: 扫空在持续扫描模式下每几秒来一次, 刷屏;
                // 进度由客户端的 .log("第 N 轮没扫到…")负责交代。
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
                // 设备确认连上了才记: "曾经输入过"不算数, "真的可用"才算。
                let ssid = ssidField.stringValue.trimmingCharacters(in: .whitespaces)
                if !ssid.isEmpty {
                    SavedWifi.remember(ssid: ssid, password: passwordField.stringValue)
                    refreshSavedNetworks()
                    append("已记住 \(ssid), 下次从这里直接选")
                }
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
