// tools/menubar/Sources/Paths.swift
//
// 应用运行期的路径与偏好设置。
//
// 这里刻意**不**把仓库路径写死在 Swift 源码里: 编译时由 build.sh 生成
// BuildConfig.swift 注入一个默认值, 用户也可以在菜单里改, 改动进 UserDefaults。

import Foundation

enum PrefKey {
    static let repoPath = "repoPath"
    static let pythonPath = "pythonPath"
    static let watchCodex = "watchCodex"
}

enum BridgePaths {
    /// ~/Library/Application Support/CodexPetBridge
    static var supportDir: URL {
        let base = FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory())
                .appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("CodexPetBridge", isDirectory: true)
    }

    static var socketPath: String { supportDir.appendingPathComponent("bridge.sock").path }
    static var logPath: String { supportDir.appendingPathComponent("bridge.log").path }

    @discardableResult
    static func ensureSupportDir() -> Bool {
        do {
            try FileManager.default.createDirectory(at: supportDir,
                                                    withIntermediateDirectories: true)
            return true
        } catch {
            return false
        }
    }
}

/// 运行期设置: 仓库位置、python、桥接端口。
///
/// 端口从固件配置头里读, 而不是在这里另写一份 —— 设备端的 PET_BRIDGE_PORT 改了,
/// 应用跟着改, 不会出现"两边各记一个端口"的漂移。
struct BridgeSettings {
    var repoPath: String
    var pythonPath: String
    var port: Int
    var watchCodex: Bool

    static let defaultPort = 8765

    static func load() -> BridgeSettings {
        let defaults = UserDefaults.standard

        var repo = defaults.string(forKey: PrefKey.repoPath) ?? ""
        if repo.isEmpty { repo = BuildConfig.repoPath }

        var python = defaults.string(forKey: PrefKey.pythonPath) ?? ""
        if python.isEmpty { python = Self.findPython() }

        let watch: Bool
        if defaults.object(forKey: PrefKey.watchCodex) == nil {
            watch = true
        } else {
            watch = defaults.bool(forKey: PrefKey.watchCodex)
        }

        return BridgeSettings(repoPath: repo,
                              pythonPath: python,
                              port: readPort(repoPath: repo),
                              watchCodex: watch)
    }

    var scriptPath: String {
        URL(fileURLWithPath: repoPath).appendingPathComponent("tools/pet_bridge.py").path
    }

    var scriptExists: Bool {
        FileManager.default.isReadableFile(atPath: scriptPath)
    }

    /// 从固件的 pet_config.h 读端口, 读不到就退回默认值。
    static func readPort(repoPath: String) -> Int {
        let header = URL(fileURLWithPath: repoPath)
            .appendingPathComponent("main/pet_config.h")
        guard let text = try? String(contentsOf: header, encoding: .utf8) else {
            return defaultPort
        }
        for line in text.split(separator: "\n") {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard trimmed.hasPrefix("#define"),
                  let range = trimmed.range(of: "PET_BRIDGE_PORT") else { continue }
            let tail = trimmed[range.upperBound...]
                .trimmingCharacters(in: .whitespaces)
            let digits = tail.prefix { $0.isNumber }
            if let value = Int(digits), value > 0, value < 65536 { return value }
        }
        return defaultPort
    }

    /// 找 python3。GUI 应用从 Finder 启动时 PATH 很短, 所以按固定顺序探测而不是
    /// 依赖 env。
    ///
    /// 从"换宠物"这一步开始, 桥接不再只用标准库了: 把图集打成 `.pet` 需要 Pillow。
    /// 所以这里**优先挑能 import PIL 的那一个**。挑错了不会在启动时报错 —— 桥接照样
    /// 跑起来, 只有点"换宠物"时才失败, 从那个现象很难反推回"是解释器选的是系统
    /// python"。探测过一次就记住, 别在每次刷新菜单时再 fork 一遍。
    static func findPython() -> String {
        if let cached = cachedPython { return cached }
        let candidates = [
            "/opt/homebrew/bin/python3",
            "/usr/local/bin/python3",
            "/usr/bin/python3",
        ].filter { FileManager.default.isExecutableFile(atPath: $0) }
        // 谁有 Pillow 用谁; 都没有就先挑第一个(至少桥接本体能跑), 菜单会提示打包不可用。
        let chosen = candidates.first(where: canImportPillow) ?? candidates.first ?? "/usr/bin/python3"
        cachedPython = chosen
        return chosen
    }

    private static var cachedPython: String?

    /// fork 一个短命子进程问一句 "import PIL 行不行"。
    static func canImportPillow(_ python: String) -> Bool {
        guard FileManager.default.isExecutableFile(atPath: python) else { return false }
        let task = Process()
        task.executableURL = URL(fileURLWithPath: python)
        task.arguments = ["-c", "import PIL"]
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        do {
            try task.run()
        } catch {
            return false
        }
        // python 启动正常在 100 ms 以内; 卡住就别把菜单拖在这儿。
        let deadline = Date().addingTimeInterval(5)
        while task.isRunning && Date() < deadline {
            usleep(20_000)
        }
        if task.isRunning {
            task.terminate()
            return false
        }
        return task.terminationStatus == 0
    }
}
