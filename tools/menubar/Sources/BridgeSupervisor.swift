// tools/menubar/Sources/BridgeSupervisor.swift
//
// 管住 pet_bridge.py 子进程, 并把控制通道的事件汇成一份可显示的状态。
//
// 应用自己不重写桥接逻辑: 发往设备的协议实现只有 tools/pet_bridge.py 一份, 那边
// 已经和设备侧解析器做过两端互校。这里只做监管(supervise) + 转发命令。

import Foundation

/// 桥接当前状态。全部来自控制通道的事件 —— 应用不推测, 只转述。
struct BridgeSnapshot {
    var bridgePid: Int32?
    var controlConnected = false
    var deviceConnected = false
    var devicePeer = ""
    var firmware = ""
    var petPackage = ""
    var codexState = "idle"
    var bubbleText = ""
    var stateSource = ""
    var sessionName = ""
    var watchingCodex = true
    var lastEventKind = ""
    var lastEventAt: Date?
    var lastError: String?

    var bridgeRunning: Bool { bridgePid != nil }

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

    // MARK: - 子进程

    private func launchProcess() {
        let settings = BridgeSettings.load()
        snapshot.watchingCodex = settings.watchCodex
        snapshot.lastError = nil

        guard settings.scriptExists else {
            snapshot.lastError = "找不到 \(settings.scriptPath), 请在菜单里重新选择项目目录"
            publish()
            return
        }

        let task = Process()
        task.executableURL = URL(fileURLWithPath: settings.pythonPath)
        var arguments = [settings.scriptPath,
                         "--port", String(settings.port),
                         "--control", BridgePaths.socketPath]
        if !settings.watchCodex { arguments.append("--no-codex") }
        task.arguments = arguments
        task.currentDirectoryURL = URL(fileURLWithPath: settings.repoPath)

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
                if !snapshot.deviceConnected { snapshot.bubbleText = "" }

            case "state":
                if let state = event["state"] as? String { snapshot.codexState = state }
                snapshot.bubbleText = (event["text"] as? String) ?? ""
                snapshot.stateSource = (event["source"] as? String) ?? ""

            case "text":
                snapshot.bubbleText = (event["text"] as? String) ?? ""

            case "session":
                snapshot.sessionName = (event["name"] as? String) ?? ""

            case "watch":
                snapshot.watchingCodex = (event["enabled"] as? Bool) ?? true

            case "device":
                let kind = (event["kind"] as? String) ?? ""
                if let fw = event["fw"] as? String { snapshot.firmware = fw }
                if let pet = event["pet"] as? String { snapshot.petPackage = pet }
                snapshot.lastEventKind = "device:\(kind)"

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
}
