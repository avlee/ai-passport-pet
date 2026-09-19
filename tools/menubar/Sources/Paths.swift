// tools/menubar/Sources/Paths.swift
//
// 应用运行期的路径与偏好设置。
//
// 这里刻意**不**把仓库路径写死, 也**不**在运行期去读源码目录 —— 应用装到别人机器上时
// 根本没有那个目录。它需要的一切都在构建期定好: 桥接脚本随包带进来, 端口由 build.sh
// 从固件头文件提取后编译进去。仓库位置只对 `build.sh` 有意义, 对应用没有意义。
//
// 早期版本让应用在运行期去仓库里找脚本和端口, 好让"改完 ping 一下"的开发回路快一点。
// 那条回路换来的是一个只在作者机器上有意义的运行时开关: 判断逻辑要分两种来源、要跟
// 用户解释"现在跑的是哪一份", 还得容忍仓库被移走。这些复杂度都不该出现在一个装好的
// 应用里 —— 开发回路改由 `build.sh --install` 承担, 它在构建期, 干净得多。

import Foundation

enum PrefKey {
    /// 解释器覆盖项。**界面上没有修改入口** —— 路径写错了很难自己发现, 改回原样更无从下手,
    /// 所以它只在「关于」里显示, 要动用命令行:
    ///
    /// ```bash
    /// defaults write local.codex-pet.bridge pythonPath /path/to/python3   # 指定
    /// defaults delete local.codex-pet.bridge pythonPath                   # 回到自动探测
    /// ```
    ///
    /// 指到不可执行的路径时会被**忽略并回落自动探测**, 不会让桥接启动失败。
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

    /// 应用包内那份桥接脚本的目录, 由 `build.sh` 在构建时复制进去。
    static var bundledBridgeDir: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("bridge", isDirectory: true)
    }

    /// 从包内取桥接脚本。返回 nil 表示这个包没带脚本 —— 只可能发生在手工动过
    /// `Contents/Resources`、或者拿着一个残缺的包的情况下。
    ///
    /// 注意 `gen_pet_package.py` 必须与它**同目录**: `pet_bridge.py` 里没有任何
    /// `sys.path` 操作, 靠的是 `sys.path[0]`(脚本自己所在目录)。所以两者在
    /// `build.sh` 里是被一起复制进同一个 `bridge/` 的。
    static func bundledScript(named name: String = "pet_bridge.py") -> String? {
        guard let dir = bundledBridgeDir else { return nil }
        let path = dir.appendingPathComponent(name).path
        return FileManager.default.isReadableFile(atPath: path) ? path : nil
    }
}

/// 运行期设置: python 解释器、桥接端口、是否跟随 Codex 会话。
struct BridgeSettings {
    /// **实际**用来启动桥接的解释器。不可执行的覆盖项会被忽略, 所以它一定是能跑的。
    var pythonPath: String
    /// 偏好里手写的那条解释器路径(没写就是 nil)。只为「关于」与诊断信息里的说明服务 ——
    /// 启动逻辑只看 `pythonPath`。
    var pythonOverride: String?
    var port: Int
    var watchCodex: Bool
    /// 桥接脚本的绝对路径。**只有应用包内这一个来源** —— 没有"仓库里那份"可选, 也就
    /// 不需要再跟用户解释当前跑的是哪一份。为 nil 表示包不完整。
    var scriptPath: String?
    /// 脚本所在目录, 同时是子进程的工作目录。跟着 `scriptPath` 一起解析出来。
    var bridgeDir: String?

    /// 桥接端口在**编译期**由 `build.sh` 从 `main/pet_config.h` 的 `PET_BRIDGE_PORT`
    /// 提取后注入。改那个宏之后要重新 `build.sh` —— 这是拿掉"运行期读源码目录"必须付的
    /// 代价, 换来的是应用不再依赖仓库还在原处。
    ///
    /// 不硬编码 8765: 端口对不上时的表现是"设备一直连不上", 很难反推回这里。
    static let defaultPort = BuildConfig.bridgePort > 0 ? BuildConfig.bridgePort : 8765

    static func load() -> BridgeSettings {
        let defaults = UserDefaults.standard

        // 覆盖项只在"确实可执行"时才作数。卸掉一个虚拟环境、或者手写时打错一个字符,
        // 都不该表现成"桥接起不来、只留一行 lastError" —— 那是从现象查不回原因的坑。
        let raw = (defaults.string(forKey: PrefKey.pythonPath) ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let override = raw.isEmpty ? nil : raw
        let usable = override.flatMap {
            FileManager.default.isExecutableFile(atPath: $0) ? $0 : nil
        }

        let watch: Bool
        if defaults.object(forKey: PrefKey.watchCodex) == nil {
            watch = true
        } else {
            watch = defaults.bool(forKey: PrefKey.watchCodex)
        }

        // 覆盖项被忽略时也把它记下来, 好让「关于」能说明白"你指的那个没在用了"。
        let script = BridgePaths.bundledScript()
        return BridgeSettings(
            pythonPath: usable ?? Self.findPython(),
            pythonOverride: override,
            port: defaultPort,
            watchCodex: watch,
            scriptPath: script,
            bridgeDir: script.map { URL(fileURLWithPath: $0).deletingLastPathComponent().path })
    }

    /// 解释器从哪来, 一句话。给「关于」和诊断信息用。
    var pythonSourceLabel: String {
        guard let override = pythonOverride else { return "自动探测" }
        return override == pythonPath ? "手动指定" : "手动指定的路径不可执行，已回落自动探测"
    }

    /// 解释器有没有 Pillow —— 决定"换宠物"能不能把图集打成 `.pet`。这里现问一句, 只给
    /// 「关于」用; 菜单那条常显路径已经拿掉了, 不值得为它每次刷新都 fork。
    var pythonHasPillow: Bool { Self.canImportPillow(pythonPath) }

    var scriptExists: Bool { scriptPath != nil }

    /// 子进程的工作目录。必须是**真实存在**的目录: `Process` 在 chdir 失败时会直接
    /// 抛错, 连一行日志都进不了 `bridge.log`, 排查时很难看出是工作目录的问题。
    /// 所以两步兜底: 脚本所在目录 → 支持目录。
    var workingDirectory: String {
        if let dir = bridgeDir, FileManager.default.fileExists(atPath: dir) {
            return dir
        }
        return BridgePaths.supportDir.path
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
        // 谁有 Pillow 用谁; 都没有就先挑第一个(至少桥接本体能跑), 「宠物」子菜单与
        // 「关于」会说明打包不可用。
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
