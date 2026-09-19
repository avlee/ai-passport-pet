// tools/menubar/Sources/ControlClient.swift
//
// 与 `pet_bridge.py --control` 的本地通道对话: AF_UNIX + JSON 行(每行一个对象)。
//
// 这是一条**事件流**, 不是一问一答: 命令发出去后结果以事件广播回来, 所以这里只暴露
// onEvent, 刻意不提供"发一条等一条"的接口 —— 回执和广播会交错在同一条流里。

import Foundation

final class ControlClient {
    private let queue = DispatchQueue(label: "codex-pet-bridge.control")
    private let retryDelay: TimeInterval = 0.5

    // 以下三项只在 queue 上访问。
    private var fd: Int32 = -1
    private var source: DispatchSourceRead?
    private var buffer = Data()
    private var path = ""
    private var wanted = false

    /// 收到一条事件(主线程回调)。
    var onEvent: (([String: Any]) -> Void)?
    /// 通道开/关(主线程回调)。
    var onConnectionChanged: ((Bool) -> Void)?

    /// 只在主线程读写。
    private(set) var connected = false

    /// 开始连接, 断了会自动重连, 直到 stop()。
    func start(path: String) {
        queue.async { [weak self] in
            guard let self else { return }
            self.path = path
            self.wanted = true
            self.tryOpen()
        }
    }

    func stop() {
        queue.async { [weak self] in
            guard let self else { return }
            self.wanted = false
            self.closeNow()
            self.path = ""
        }
    }

    /// 发一条命令(不等回执)。没连上时静默丢弃。
    func send(_ object: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: object) else { return }
        queue.async { [weak self] in
            guard let self, self.fd >= 0 else { return }
            var line = data
            line.append(0x0A)
            let descriptor = self.fd
            line.withUnsafeBytes { raw in
                guard let base = raw.baseAddress else { return }
                var written = 0
                while written < raw.count {
                    let count = write(descriptor, base + written, raw.count - written)
                    if count > 0 {
                        written += count
                    } else if errno == EAGAIN || errno == EWOULDBLOCK {
                        return  // 控制消息都是小包, 真塞满就丢这一条, 不值得阻塞。
                    } else {
                        return
                    }
                }
            }
        }
    }

    // MARK: - queue 上执行

    private func tryOpen() {
        guard wanted else { return }
        closeNow()

        let descriptor = socket(AF_UNIX, SOCK_STREAM, 0)
        guard descriptor >= 0 else {
            scheduleRetry()
            return
        }

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else {
            close(descriptor)
            return  // 路径超长是配置问题, 重试没有意义。
        }
        withUnsafeMutableBytes(of: &addr.sun_path) { raw in
            raw.copyBytes(from: bytes)
        }

        let result = withUnsafePointer(to: &addr) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(descriptor, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard result == 0 else {
            close(descriptor)
            scheduleRetry()
            return
        }

        let flags = fcntl(descriptor, F_GETFL, 0)
        _ = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK)

        fd = descriptor
        buffer.removeAll()

        let readSource = DispatchSource.makeReadSource(fileDescriptor: descriptor,
                                                       queue: queue)
        readSource.setEventHandler { [weak self] in self?.drain() }
        // 关闭交给 cancel handler, 这样 fd 号在本块结束前都还被占着, 不会被新连接
        // 复用 —— 否则 cancel handler 可能关掉刚建好的那条。
        readSource.setCancelHandler { close(descriptor) }
        source = readSource
        readSource.resume()

        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if !self.connected {
                self.connected = true
                self.onConnectionChanged?(true)
            }
        }
    }

    /// 关掉当前连接(不安排重连)。
    private func closeNow() {
        if let readSource = source {
            source = nil
            fd = -1
            readSource.cancel()
        } else if fd >= 0 {
            close(fd)
            fd = -1
        }
        buffer.removeAll()

        DispatchQueue.main.async { [weak self] in
            guard let self, self.connected else { return }
            self.connected = false
            self.onConnectionChanged?(false)
        }
    }

    private func scheduleRetry() {
        guard wanted else { return }
        queue.asyncAfter(deadline: .now() + retryDelay) { [weak self] in
            guard let self, self.wanted, self.fd < 0 else { return }
            self.tryOpen()
        }
    }

    private func drain() {
        guard fd >= 0 else { return }

        var chunk = [UInt8](repeating: 0, count: 4096)
        while true {
            let count = read(fd, &chunk, chunk.count)
            if count > 0 {
                buffer.append(contentsOf: chunk[0..<count])
            } else if count == 0 {
                closeNow()
                scheduleRetry()
                return
            } else if errno == EAGAIN || errno == EWOULDBLOCK {
                break
            } else {
                closeNow()
                scheduleRetry()
                return
            }
        }

        while let index = buffer.firstIndex(of: 0x0A) {
            let line = Data(buffer[buffer.startIndex..<index])
            buffer.removeSubrange(buffer.startIndex...index)
            guard !line.isEmpty, let object = try? JSONSerialization
                .jsonObject(with: line) as? [String: Any] else { continue }
            DispatchQueue.main.async { [weak self] in
                self?.onEvent?(object)
            }
        }
    }
}
