// tools/menubar/Sources/BridgeSupervisor.swift
//
// 管住 pet_bridge.py 子进程, 并把控制通道的事件汇成一份可显示的状态。
//
// 应用自己不重写桥接逻辑: 发往设备的协议实现只有 tools/pet_bridge.py 一份, 那边
// 已经和设备侧解析器做过两端互校。这里只做监管(supervise) + 转发命令。

import Foundation

/// 一只可换的宠物。列表由桥接侧扫 `~/.codex/pets` 得到 —— 设备那条 pets 分区只装
/// 得下一只, 所以"换宠物"就是把这边的另一只重新打包推下去。
struct PetEntry {
    let id: String
    let display: String
    /// 已经打包过(缓存里有 .pet)。没打包过也没关系, 点了才开始打。
    let cached: Bool
    let size: Int
    /// 这只不能用时的原因(图集版本不对、pet.json 读不出来…)。
    let problem: String?
}

/// 最近一次换宠物的进展。
///
/// 注意"发完了"不等于"装上了": 包发完之后设备还要核对长度与整包 CRC 再重新映射,
/// 然后回一条 petdone。所以 sent 只是个中间态, 结论只在 done/failed。
struct PetTransfer {
    var id = ""
    var state = "idle"   // idle / preparing / sending / sent / done / failed
    var sent = 0
    var total = 0
    var message: String?

    var isActive: Bool { state == "preparing" || state == "sending" }

    /// 顶栏那一行用的极短说法。空闲时返回 nil。
    var shortLabel: String? {
        switch state {
        case "preparing": return "打包中…"
        case "sending":
            return total > 0 ? "传输 \(min(100, sent * 100 / total))%" : "传输中…"
        case "sent": return "等设备确认…"
        default: return nil
        }
    }

    /// 菜单里那一行。idle 时返回 nil, 那一行就藏起来。
    var label: String? {
        switch state {
        case "preparing":
            return "正在打包 \(id)…"
        case "sending":
            guard total > 0 else { return "正在传输 \(id)…" }
            let percent = min(100, sent * 100 / total)
            return String(format: "正在传输 %@：%d%%（%.1f / %.1f MiB）", id, percent,
                          Double(sent) / 1_048_576, Double(total) / 1_048_576)
        case "sent":
            return "已发完 \(id)，等设备确认…"
        case "done":
            return "已装上 \(id)"
        case "failed":
            return "⚠︎ 换宠物失败：\(message ?? id)"
        default:
            return nil
        }
    }
}

/// 桥接当前状态。全部来自控制通道的事件 —— 应用不推测, 只转述。
struct BridgeSnapshot {
    var bridgePid: Int32?
    var controlConnected = false
    var deviceConnected = false
    var devicePeer = ""
    var firmware = ""
    var petPackage = ""
    var batterySoc: Int?
    var codexState = "idle"
    var bubbleText = ""
    var stateSource = ""
    var sessionName = ""
    var watchingCodex = true
    var lastEventKind = ""
    var lastEventAt: Date?
    var lastError: String?

    // 宠物库(桥接侧扫 ~/.codex/pets 的结果)。petsAvailable 为假表示桥接启动时带
    // 了 --no-pets, 或者还没收到过 pets 事件。
    var pets: [PetEntry] = []
    var petsDir = ""
    var petsAvailable = false
    var petBusy = false
    var petTransfer = PetTransfer()
    /// 桥接那个解释器能不能打包(有没有 Pillow)。nil 表示还没收到过 pets 事件。
    var petPackAvailable: Bool?
    var petPackHint: String?

    var bridgeRunning: Bool { bridgePid != nil }

    /// 电量说法。读不到时返回 nil(菜单里那一行直接藏起来)。
    var batteryLabel: String? {
        guard let batterySoc else { return nil }
        return "\(batterySoc)%"
    }

    /// 低电量(<=20%), 菜单里加个警示前缀。
    var batteryIsLow: Bool { (batterySoc ?? 100) <= 20 }

    /// Codex 状态的中文说法。
    var codexStateLabel: String {
        switch codexState {
        case "working": return "工作中"
        case "waiting": return "等待确认"
        case "ready": return "已完成"
        case "failed": return "出错了"
        case "idle": return "空闲"
        default: return codexState
        }
    }
}

final class BridgeSupervisor {
    private(set) var snapshot: BridgeSnapshot
    /// 状态变化(主线程)。
    var onChange: ((BridgeSnapshot) -> Void)?

    private let client = ControlClient()
    private var process: Process?
    private var logHandle: FileHandle?
    private var quitting = false
    private var restarts = 0

    init(settings: BridgeSettings) {
        snapshot = BridgeSnapshot()
        snapshot.watchingCodex = settings.watchCodex
        client.onEvent = { [weak self] event in self?.apply(event: event) }
        client.onConnectionChanged = { [weak self] connected in
            guard let self else { return }
            self.snapshot.controlConnected = connected
            if connected {
                // 通道建好后再施加"是否跟随日志"这个偏好。刻意不用 --no-codex 启动:
                // 那样桥接侧根本不会创建 watcher, 菜单里的开关就再也打不开了。
                self.client.send(["cmd": "watch", "enabled": self.snapshot.watchingCodex])
            } else {
                self.snapshot.deviceConnected = false
            }
            self.publish()
        }
    }

    // MARK: - 生命周期

    func start() {
        quitting = false
        restarts = 0
        BridgePaths.ensureSupportDir()
        launchProcess()
        client.start(path: BridgePaths.socketPath)
    }

    func stop() {
        quitting = true
        client.stop()
        terminateProcess()
    }

    func restart() {
        terminateProcess()
        snapshot = BridgeSnapshot()
        snapshot.watchingCodex = BridgeSettings.load().watchCodex
        client.stop()
        // 让 python 有机会释放端口再拉起, 否则新进程会撞上 "Address already in use"。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
            self?.start()
        }
    }

    // MARK: - 命令

    func pushState(_ state: String, text: String? = nil) {
        var command: [String: Any] = ["cmd": "state", "state": state]
        if let text, !text.isEmpty { command["text"] = text }
        client.send(command)
    }

    func sendText(_ text: String) {
        client.send(["cmd": "text", "text": text])
    }

    func setWatchingCodex(_ enabled: Bool) {
        UserDefaults.standard.set(enabled, forKey: PrefKey.watchCodex)
        snapshot.watchingCodex = enabled
        client.send(["cmd": "watch", "enabled": enabled])
        publish()
    }

    func requestSnapshot() {
        client.send(["cmd": "snapshot"])
    }

    /// 重新扫一遍 ~/.codex/pets。用户刚在 Codex 里下了新宠物时不用重启桥接。
    func refreshPets() {
        client.send(["cmd": "petlist"])
    }

    /// 换宠物。传输在桥接的后台线程里跑, 这里立刻返回 —— 结果走 petxfer/petdone
    /// 事件, 不要在这里等。
    func installPet(_ id: String) {
        client.send(["cmd": "pet", "id": id])
    }

    // MARK: - 子进程

    private func launchProcess() {
        let settings = BridgeSettings.load()
        snapshot.watchingCodex = settings.watchCodex
        snapshot.lastError = nil

        // 桥接脚本只有一个来源: 应用包内。正常构建出来的包一定带着
        // bridge/pet_bridge.py, 走到这里说明包不完整。
        guard let scriptPath = settings.scriptPath else {
            snapshot.lastError = "应用包内没有 bridge/pet_bridge.py —— 这个包不完整，"
                + "请重新用 tools/menubar/build.sh 构建"
            publish()
            return
        }

        let task = Process()
        task.executableURL = URL(fileURLWithPath: settings.pythonPath)
        var arguments = [scriptPath,
                         "--port", String(settings.port),
                         "--control", BridgePaths.socketPath]
        if !settings.watchCodex { arguments.append("--no-codex") }
        task.arguments = arguments
        // 工作目录必须真实存在, 否则 Process 会在 chdir 上直接抛错。
        task.currentDirectoryURL = URL(fileURLWithPath: settings.workingDirectory)

        if let handle = openLogFile() {
            logHandle = handle
            task.standardOutput = handle
            task.standardError = handle
        } else {
            logHandle = nil
        }

        task.terminationHandler = { [weak self] finished in
            DispatchQueue.main.async {
                guard let self else { return }
                self.process = nil
                self.snapshot.bridgePid = nil
                self.snapshot.controlConnected = false
                self.snapshot.deviceConnected = false
                if !self.quitting {
                    let tail = self.logTail(lines: 2)
                    self.snapshot.lastError = tail.isEmpty
                        ? "桥接进程退出(状态码 \(finished.terminationStatus))"
                        : tail
                    self.publish()
                    self.scheduleRelaunch()
                } else {
                    self.publish()
                }
            }
        }

        do {
            try task.run()
        } catch {
            snapshot.lastError = "无法启动 \(settings.pythonPath): \(error.localizedDescription)"
            publish()
            return
        }

        process = task
        snapshot.bridgePid = task.processIdentifier
        publish()
    }

    private func terminateProcess() {
        guard let task = process, task.isRunning else { return }
        task.terminate()
        process = nil
        snapshot.bridgePid = nil
    }

    /// 子进程意外退出后拉起来。退避到最长 15 秒, 免得配错路径时 1 秒一次刷屏。
    private func scheduleRelaunch() {
        restarts += 1
        let delay = min(pow(2.0, Double(min(restarts, 4))), 15.0)
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, !self.quitting, self.process == nil else { return }
            self.launchProcess()
            self.client.start(path: BridgePaths.socketPath)
        }
    }

    private func openLogFile() -> FileHandle? {
        let path = BridgePaths.logPath
        let manager = FileManager.default
        if !manager.fileExists(atPath: path) {
            manager.createFile(atPath: path, contents: nil)
        }
        guard let handle = FileHandle(forWritingAtPath: path) else { return nil }
        handle.seekToEndOfFile()
        let stamp = ISO8601DateFormatter().string(from: Date())
        if let banner = "\n===== 启动桥接 \(stamp) =====\n".data(using: .utf8) {
            handle.write(banner)
        }
        return handle
    }

    /// 读日志尾部, 用于在菜单里显示最后一条报错。
    func logTail(lines: Int) -> String {
        guard let text = try? String(contentsOfFile: BridgePaths.logPath,
                                      encoding: .utf8) else { return "" }
        let all = text.split(separator: "\n", omittingEmptySubsequences: true)
        return all.suffix(lines).joined(separator: "\n")
    }

    // MARK: - 事件

    private func apply(event: [String: Any]) {
        if let name = event["event"] as? String {
            snapshot.lastEventKind = name
            switch name {
            case "bridge":
                if let pid = event["pid"] as? Int { snapshot.bridgePid = Int32(pid) }
                if let codex = event["codex"] as? Bool { snapshot.watchingCodex = codex }
                snapshot.lastError = nil

            case "link":
                snapshot.deviceConnected = (event["connected"] as? Bool) ?? false
                snapshot.devicePeer = (event["peer"] as? String) ?? ""
                if !snapshot.deviceConnected {
                    snapshot.bubbleText = ""
                    // 设备走了, 最后那个读数就不再是"当前电量", 别挂在那里骗人。
                    snapshot.batterySoc = nil
                }

            case "state":
                if let state = event["state"] as? String { snapshot.codexState = state }
                snapshot.bubbleText = (event["text"] as? String) ?? ""
                snapshot.stateSource = (event["source"] as? String) ?? ""

            case "text":
                snapshot.bubbleText = (event["text"] as? String) ?? ""

            case "battery":
                // soc 为 null 表示设备读不到电量; JSONSerialization 给的也是 NSNull,
                // 转 Int 失败, 正好落到 nil。
                snapshot.batterySoc = event["soc"] as? Int

            case "session":
                snapshot.sessionName = (event["name"] as? String) ?? ""

            case "watch":
                snapshot.watchingCodex = (event["enabled"] as? Bool) ?? true

            case "device":
                let kind = (event["kind"] as? String) ?? ""
                if let fw = event["fw"] as? String { snapshot.firmware = fw }
                if let pet = event["pet"] as? String { snapshot.petPackage = pet }
                snapshot.lastEventKind = "device:\(kind)"

            // --- 宠物库 ---
            // 三件事分开记, 不要塞进 device: ControlHub 每类事件只留最新一份,
            // 混在一起会把 hello 报上来的固件/宠物包顶掉。
            case "pets":
                snapshot.petsAvailable = true
                snapshot.petsDir = (event["pets_dir"] as? String) ?? ""
                snapshot.petBusy = (event["busy"] as? Bool) ?? false
                // 打包能力: 桥接跑在哪个解释器上、那个解释器有没有 Pillow。缺 Pillow 时
                // 点任何一只都会失败, 所以菜单要在点之前就说明白。
                if let packer = event["packer"] as? [String: Any] {
                    snapshot.petPackAvailable = (packer["available"] as? Bool) ?? false
                    snapshot.petPackHint = packer["hint"] as? String
                } else {
                    snapshot.petPackAvailable = true   // 老桥接没这个字段: 别平白报错
                    snapshot.petPackHint = nil
                }
                snapshot.pets = (event["items"] as? [[String: Any]] ?? []).map { item in
                    PetEntry(id: (item["id"] as? String) ?? "",
                             display: (item["display"] as? String) ?? "",
                             cached: (item["cached"] as? Bool) ?? false,
                             size: (item["size"] as? Int) ?? 0,
                             problem: item["error"] as? String)
                }
                // last 是桥接侧记住的上一次结局, 客户端中途接入时靠它对齐进度行。
                if let last = event["last"] as? [String: Any] { applyPetTransfer(last) }

            case "petxfer":
                applyPetTransfer(event)
                snapshot.petBusy = snapshot.petTransfer.isActive

            case "petdone":
                // 设备确认。成功时 id 是槽里**真正**装上的那只, 与 hello 报的是同一个
                // 来源, 所以直接信它; 失败时桥接侧给的是原本还在槽里那只的 id。
                let id = (event["id"] as? String) ?? ""
                let ok = (event["ok"] as? Bool) ?? false
                if !id.isEmpty { snapshot.petTransfer.id = id }
                snapshot.petTransfer.state = ok ? "done" : "failed"
                snapshot.petTransfer.message = ok ? nil : "设备拒收了这个包"
                snapshot.petBusy = false
                if ok { snapshot.petPackage = id }
                snapshot.lastEventKind = "petdone:\(ok ? "ok" : "failed")"

            case "error":
                snapshot.lastError = (event["message"] as? String) ?? "控制通道报错"

            case "snapshot":
                // 服务端在客户端接入时给的全量状态: 逐个合并, 与增量事件同一套映射。
                if let state = event["state"] as? [String: Any] {
                    for key in state.keys.sorted() {
                        if let inner = state[key] as? [String: Any] { apply(event: inner) }
                    }
                }

            default:
                break
            }
            snapshot.lastEventAt = Date()
        }
        publish()
    }

    private func publish() {
        onChange?(snapshot)
    }

    /// 换宠物进度。petxfer 事件与 pets 事件里的 last 字段是同一套键, 合并到一处。
    private func applyPetTransfer(_ payload: [String: Any]) {
        if let id = payload["id"] as? String { snapshot.petTransfer.id = id }
        if let state = payload["state"] as? String { snapshot.petTransfer.state = state }
        if let sent = payload["sent"] as? Int { snapshot.petTransfer.sent = sent }
        if let total = payload["total"] as? Int { snapshot.petTransfer.total = total }
        if let message = payload["message"] as? String {
            snapshot.petTransfer.message = message
        }
    }
}
