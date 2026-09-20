// tools/menubar/Sources/ProvisionCommand.swift
//
// `--provision` 无界面模式。
//
// 为什么要有它
// ------------
// 菜单栏窗口是给用户用的, 但写代码的人看不到菜单栏 —— 这条链路要能被自动化验证,
// 就必须有一个能跑、能打印、能返回退出码的入口。顺带也让它能被脚本调用(比如给
// 多台设备批量配网)。
//
// 退出码: 0 成功, 1 失败, 2 参数错误。

import Foundation

enum ProvisionCommand {
    private static let usage = """
    用法: CodexPetBridge --provision --pin 1234 [选项]

      --pin <4位>        设备屏幕上显示的配对码(除 --scan 外必填)
      --pin-file <路径>  从这个文件轮询配对码, 代替 --pin —— 给不方便手抄屏幕的
                         场景用(例如远程会话里跑自动化): 由串口监听方把设备屏幕上的
                         码写进文件, CLI 每 200ms 看一眼, 拿到就立刻配对
      --ssid <名称>      2.4G 网络名称(默认取当前 Wi-Fi)
      --password <密码>  Wi-Fi 密码
      --host <地址>      配对端地址, 默认本机局域网地址
      --port <端口>      配对端端口, 默认读固件配置
      --scan             只扫描并列出配网中的设备
      --scan-window <秒> 扫描多久后选信号最强的一台(默认 4)
      --timeout <秒>     整个流程的超时(默认 60)
      --forget           清空设备里已保存的网络参数(需要 --pin)
      --json             逐行输出 JSON, 便于脚本消费
      --out <路径>       同时把输出追加到这个文件

    --out 是为一种特殊情况准备的: 蓝牙权限(TCC)按"责任进程"判定, 而从终端/SDK
    里拉起的子进程, 责任进程是终端本身 —— 终端没有 NSBluetoothAlwaysUsageDescription,
    进程会直接 SIGABRT。用 `open -n ... --args` 启动时责任进程才是应用自己, 但那样
    stdout 拿不到, 所以让它把结果写进文件, 由调用方轮询。
    """

    private struct Options {
        var pin = ""
        var pinFile = ""
        var ssid = ""
        var password = ""
        var host = ""
        var port = 0
        var scanOnly = false
        var forget = false
        var json = false
        var scanWindow: Double = 4
        var timeout: Double = 60
        var outPath = ""
    }

    /// --out 打开的文件句柄。进程内只可能有一份, 用 static 省得一路传参。
    private static var outHandle: FileHandle?

    private static func writeToOut(_ line: String) {
        guard let handle = outHandle else { return }
        handle.write(Data((line + "\n").utf8))
        // 调用方是边写边读的轮询者, 不 flush 它会一直看到空文件。
        try? handle.synchronize()
    }

    private static func parse(_ arguments: [String]) -> Options? {
        var options = Options()
        var index = 0

        func next(_ flag: String) -> String? {
            guard index + 1 < arguments.count else {
                FileHandle.standardError.write(Data("\(flag) 后面缺少值\n".utf8))
                return nil
            }
            index += 1
            return arguments[index]
        }

        while index < arguments.count {
            let flag = arguments[index]
            switch flag {
            case "--pin": options.pin = next(flag) ?? ""
            case "--pin-file": options.pinFile = next(flag) ?? ""
            case "--ssid": options.ssid = next(flag) ?? ""
            case "--password": options.password = next(flag) ?? ""
            case "--host": options.host = next(flag) ?? ""
            case "--port":
                guard let text = next(flag), let value = Int(text) else { return nil }
                options.port = value
            case "--scan-window":
                guard let text = next(flag), let value = Double(text) else { return nil }
                options.scanWindow = value
            case "--timeout":
                guard let text = next(flag), let value = Double(text) else { return nil }
                options.timeout = value
            case "--scan": options.scanOnly = true
            case "--forget": options.forget = true
            case "--json": options.json = true
            case "--out": options.outPath = next(flag) ?? ""
            case "-h", "--help":
                print(usage)
                exit(0)
            default:
                FileHandle.standardError.write(Data("不认识的参数: \(flag)\n".utf8))
                return nil
            }
            index += 1
        }
        return options
    }

    private static func emit(_ options: Options, _ event: String,
                            _ fields: [String: Any] = [:]) {
        let text: String
        if options.json {
            var payload = fields
            payload["event"] = event
            text = (try? JSONSerialization.data(withJSONObject: payload,
                                                options: [.sortedKeys]))
                .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        } else if let message = fields["message"] as? String {
            text = "[\(event)] \(message)"
        } else {
            text = "[\(event)]"
        }
        print(text)
        writeToOut(text)
        // 管道被消费方关掉时不要崩, 直接失败退出。
        fflush(stdout)
    }

    /// --pin-file: 轮询文件直到出现 4 位数字码, 然后立刻配对。
    /// 配对码每次开配网页都会重新生成(设备侧 2026-09-20 起), 但"读屏抄码"这件事没法用代码
    /// 代劳, 所以留这条路径: 由串口监听方把码写进文件, 这边以 200ms 的粒度取用。
    /// 整体超时与主等待循环共用同一个 options.timeout。
    private static func pairFromFile(_ options: Options,
                                     client: ProvisionClient,
                                     outcome: Outcome) {
        DispatchQueue(label: "provision.pinfile").async {
            let deadline = Date().addingTimeInterval(options.timeout)
            while Date() < deadline {
                let text = (try? String(contentsOfFile: options.pinFile,
                                        encoding: .utf8)) ?? ""
                let pin = text.trimmingCharacters(in: .whitespacesAndNewlines)
                if pin.count == 4, pin.allSatisfy({ $0.isNumber }) {
                    emit(options, "log", ["message": "从 \(options.pinFile) 读到配对码, 开始配对"])
                    client.pair(pin: pin)
                    return
                }
                usleep(200_000)
            }
            emit(options, "failed",
                 ["message": "pin 文件始终没有出现合法配对码: \(options.pinFile)"])
            outcome.finish(code: 1)
        }
    }

    static func run(arguments: [String]) -> Int32 {
        guard var options = parse(arguments) else {
            FileHandle.standardError.write(Data((usage + "\n").utf8))
            return 2
        }

        if options.ssid.isEmpty { options.ssid = NetworkInfo.currentSSID() ?? "" }
        if options.host.isEmpty { options.host = NetworkInfo.localIPv4() ?? "" }
        if options.port == 0 { options.port = BridgeSettings.load().port }

        // --out 用截断而不是追加: 调用方每次运行都从空文件开始读, 复用旧文件会把
        // 上一轮的结果混进来。
        if !options.outPath.isEmpty {
            FileManager.default.createFile(atPath: options.outPath, contents: nil)
            guard let handle = FileHandle(forWritingAtPath: options.outPath) else {
                FileHandle.standardError.write(
                    Data("打不开 --out 指定的文件: \(options.outPath)\n".utf8))
                return 2
            }
            outHandle = handle
        }

        if !options.scanOnly {
            if !options.pinFile.isEmpty {
                // --pin-file 模式下 --pin 允许缺省, 码是连接建立后才从文件里来的。
            } else if options.pin.count != 4 || !options.pin.allSatisfy({ $0.isNumber }) {
                FileHandle.standardError.write(Data("--pin 必须是 4 位数字\n".utf8))
                return 2
            }
            if !options.forget, options.ssid.isEmpty {
                FileHandle.standardError.write(
                    Data("读不到当前 Wi-Fi 名字, 请用 --ssid 指定\n".utf8))
                return 2
            }
            if !options.forget, options.host.isEmpty {
                FileHandle.standardError.write(
                    Data("读不到本机局域网地址, 请用 --host 指定\n".utf8))
                return 2
            }
        }

        // 所有事件都在这一条串行队列上回调, 所以 done / sentParams 不需要加锁;
        // outcome 是主线程(等待循环)也要读的, 由它自己带锁。
        let outcome = Outcome()
        let eventQueue = DispatchQueue(label: "provision.cli")
        var done = false
        var sentParams = false

        // 闭包里要用到 client 自己(配对通过后接着下发参数), 所以先声明、后赋值:
        // 隐式解包可选变量在声明时就已是 nil, 闭包捕获它是合法的。不能用
        // `let client = ProvisionClient { ... client ... }` —— 那是自引用。
        var client: ProvisionClient!
        client = ProvisionClient(scanWindow: options.scanWindow,
                                 emitQueue: eventQueue) { event in
            switch event {
            case .log(let text):
                emit(options, "log", ["message": text])

            case .scanning:
                emit(options, "scanning")

            case .found(let devices):
                if devices.isEmpty {
                    emit(options, "found", ["count": 0])
                    outcome.finish(code: 1)
                    return
                }
                for device in devices {
                    emit(options, "device", ["name": device.name, "rssi": device.rssi])
                }
                if options.scanOnly {
                    outcome.finish(code: 0)
                }

            case .connecting(let device):
                emit(options, "connecting", ["name": device.name, "rssi": device.rssi])

            case .connected(let info):
                emit(options, "connected", ["device": info["device"] as? String ?? "?",
                                            "fw": info["fw"] as? String ?? "",
                                            "pet": info["pet"] as? String ?? ""])
                if options.forget {
                    client.forget()
                } else if options.pinFile.isEmpty {
                    client.pair(pin: options.pin)
                } else {
                    pairFromFile(options, client: client, outcome: outcome)
                }

            case .status(let status):
                emit(options, "status", ["message": status.describe])
                switch status {
                case .paired:
                    // 配对通过后自动下发: 用户的意图本来就是"把这台设备接上这个
                    // Wi-Fi", 再让他点一次只是多余。
                    guard !sentParams else { return }
                    sentParams = true
                    client.apply(ssid: options.ssid, password: options.password,
                                 host: options.host, port: options.port)
                case .done(let ip):
                    emit(options, "done", ["ip": ip])
                    done = true
                    outcome.finish(code: 0)
                case .forgot:
                    outcome.finish(code: 0)
                case .deviceFailed(let reason), .deviceError(let reason):
                    emit(options, "failed", ["message": reason])
                    outcome.finish(code: 1)
                case .pinRejected(let left):
                    if left <= 0 {
                        emit(options, "failed",
                             ["message": "配对码连续错误, 设备已断开并重新生成配对码"])
                        outcome.finish(code: 1)
                    }
                default:
                    break
                }

            case .disconnected(let reason):
                if done {
                    // 配网成功后设备主动收掉蓝牙省电, 这是预期行为。
                    emit(options, "disconnected", ["message": reason, "expected": true])
                } else if !outcome.finished {
                    emit(options, "failed", ["message": "连接断开: \(reason)"])
                    outcome.finish(code: 1)
                }

            case .failed(let message):
                emit(options, "failed", ["message": message])
                outcome.finish(code: 1)
            }
        }

        client.start()

        let deadline = Date().addingTimeInterval(options.timeout)
        while !outcome.finished, Date() < deadline {
            // 不能靠 RunLoop.main 排空回调: 脚本环境里主 run loop 未必在跑, 所以
            // 事件走的是自己的队列, 这里只轮询结果。
            usleep(50_000)
        }
        if !outcome.finished {
            emit(options, "failed",
                 ["message": "超时(\(Int(options.timeout)) 秒)仍未配网完成"])
        }

        client.stop()
        return outcome.code
    }
}

/// 跨线程的完成标志 + 退出码。事件的串行队列写, 主线程轮询读。
private final class Outcome {
    private let lock = NSLock()
    private var _finished = false
    private var _code: Int32 = 1

    var finished: Bool {
        lock.lock()
        defer { lock.unlock() }
        return _finished
    }

    var code: Int32 {
        lock.lock()
        defer { lock.unlock() }
        return _code
    }

    func finish(code: Int32) {
        lock.lock()
        defer { lock.unlock() }
        _finished = true
        _code = code
    }
}
