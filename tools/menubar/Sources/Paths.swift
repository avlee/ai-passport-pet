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
    static func findPython() -> String {
        let candidates = [
            "/usr/bin/python3",
            "/opt/homebrew/bin/python3",
            "/usr/local/bin/python3",
        ]
        for path in candidates where FileManager.default.isExecutableFile(atPath: path) {
            return path
        }
        return "/usr/bin/python3"
    }
}
