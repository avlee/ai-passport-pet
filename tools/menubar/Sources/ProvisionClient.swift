// tools/menubar/Sources/ProvisionClient.swift
//
// BLE 配网的中央端。设备是 peripheral, 这里是 central。
//
// 为什么不用 Python/bleak
// ----------------------
// 菜单栏应用原本零第三方依赖(只用 swiftc + AppKit), 配网也一样用系统的
// CoreBluetooth 就够了 —— 不为一个功能给用户加一条 pip 安装。
//
// 这一层刻意**不碰 AppKit**: 同一份实现同时被菜单栏窗口和 `--provision` 无界面
// 模式使用。无界面模式不只是好玩的附加功能, 它是这条链路唯一能自动化验证的方式
// (界面在菜单栏里, 写代码的人看不到)。
//
// 协议(与 main/pet_provision.c 一致)
// ----------------------------------
// 服务/特征是我们自己的 128 位 UUID, 见下面的 UUID 常量。
//
//   cmd     写入 JSON 行      {"pin":"1234"} / {"ssid":"..."} / {"pass":"..."}
//                            {"host":"..."} / {"port":8765} / {"commit":true}
//                            {"forget":true}
//   status  通知(可读)        {"event":"connected"|"paired"|"pin_error"|"set"|
//                             "applying"|"done"|"failed"|"forgot"|"error", ...}
//   info    只读             {"device":"CodexPet-75B4","fw":...,"configured":...}
//
// 配对码由设备屏幕显示, 这里只负责把它写过去。配对码通过之后设备才接受 Wi-Fi
// 参数, 所以顺序不能反。

import CoreBluetooth
import Foundation

// 与 main/pet_provision.c 里的 PROV_UUID_* 必须逐字一致。
enum ProvisionUUID {
    static let service = CBUUID(string: "c0de0001-7e57-4a7c-9d20-706574627269")
    static let cmd = CBUUID(string: "c0de0002-7e57-4a7c-9d20-706574627269")
    static let status = CBUUID(string: "c0de0003-7e57-4a7c-9d20-706574627269")
    static let info = CBUUID(string: "c0de0004-7e57-4a7c-9d20-706574627269")
}

struct DiscoveredDevice {
    let id: UUID
    let name: String
    let rssi: Int
}

/// 设备上报的状态。`raw` 保留原始 JSON, 便于把认不出来的事件原样打日志。
enum ProvisionStatus {
    case paired
    case pinRejected(left: Int)
    case fieldSet(String)
    case applying
    case done(ip: String)
    case deviceFailed(String)
    case forgot
    case deviceError(String)
    case unknown(String, [String: Any])

    /// 给日志用的一句话。
    var describe: String {
        switch self {
        case .paired: return "配对码通过"
        case .pinRejected(let left): return "配对码不正确, 还能试 \(left) 次"
        case .fieldSet(let key): return "已收到 \(key)"
        case .applying: return "设备正在保存并连接 Wi-Fi"
        case .done(let ip): return "配网完成, 设备地址 \(ip)"
        case .deviceFailed(let reason): return "设备上报失败: \(reason)"
        case .forgot: return "已清空设备上的网络参数"
        case .deviceError(let message): return "设备拒绝: \(message)"
        case .unknown(let name, _): return "设备上报 \(name)"
        }
    }
}

enum ProvisionEvent {
    case log(String)
    case scanning
    case found([DiscoveredDevice])
    case connecting(DiscoveredDevice)
    /// 已连上、通知已打开、info 已读到 —— 这时才能写命令。
    case connected(deviceInfo: [String: Any])
    case status(ProvisionStatus)
    case disconnected(String)
    case failed(String)
}

/// 设备端 reason 码 -> 中文。设备只回简短的 ASCII 原因, 与
/// main/pet_app.c 的 provision_error_text() 一一对应。
enum ProvisionReason {
    static func localize(_ code: String) -> String {
        switch code {
        case "save failed": return "设备写入参数失败"
        case "connect failed": return "设备连不上这个网络(检查是否 2.4G、密码是否正确)"
        case "not paired": return "还没通过配对码校验"
        case "ssid missing": return "缺少网络名称"
        case "forget failed": return "清空参数失败"
        case "pin format": return "配对码必须是 4 位数字"
        case "unknown key": return "设备不认识这个字段(固件版本可能偏旧)"
        case "too long": return "这个值太长"
        case "bad value": return "这个值不合法"
        case "bad json": return "报文格式错误"
        default: return code
        }
    }
}

final class ProvisionClient: NSObject {
    private let onEvent: (ProvisionEvent) -> Void
    private let queue = DispatchQueue(label: "provision.central")
    /// 回调投递到哪条队列。默认主队列(GUI 用); 无界面模式传自己的串行队列,
    /// 这样它不必依赖主 run loop 去排空 — 脚本环境里主 run loop 不一定在跑。
    private let emitQueue: DispatchQueue

    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var cmdCharacteristic: CBCharacteristic?
    private var statusCharacteristic: CBCharacteristic?
    private var infoCharacteristic: CBCharacteristic?

    private var discovered: [UUID: (peripheral: CBPeripheral, name: String, rssi: Int)] = [:]
    private var scanTimer: DispatchSourceTimer?
    /// 扫描到多少秒之后按信号强度选一台连上去。
    private let scanWindow: TimeInterval

    /// 写入必须串行: CoreBluetooth 不会替你排队, 并发发多条 withResponse 写会丢。
    private var writeQueue: [(CBCharacteristic, Data)] = []
    private var writing = false

    private var statusBuffer = Data()
    private var finished = false

    init(scanWindow: TimeInterval = 4.0, emitQueue: DispatchQueue = .main,
         onEvent: @escaping (ProvisionEvent) -> Void) {
        self.scanWindow = scanWindow
        self.emitQueue = emitQueue
        self.onEvent = onEvent
        super.init()
    }

    private func emit(_ event: ProvisionEvent) {
        emitQueue.async { self.onEvent(event) }
    }

    // MARK: - 生命周期

    func start() {
        finished = false
        central = CBCentralManager(delegate: self, queue: queue,
                                   options: [CBCentralManagerOptionShowPowerAlertKey: true])
    }

    /// 连接信号最强的那台。等满 scanWindow 再动手, 免得连到远处那台。
    private func connectStrongest() {
        scanTimer?.cancel()
        scanTimer = nil

        let found = discovered.values
            .map { DiscoveredDevice(id: $0.peripheral.identifier, name: $0.name, rssi: $0.rssi) }
            .sorted { $0.rssi > $1.rssi }
        emit(.found(found))

        guard let best = found.first,
              let entry = discovered[best.id] else {
            emit(.failed("没有扫描到配网中的设备。确认设备停在配对码界面(长按下键可以打开)。"))
            return
        }

        central?.stopScan()
        peripheral = entry.peripheral
        peripheral?.delegate = self
        emit(.connecting(best))
        central?.connect(entry.peripheral, options: nil)
    }

    func stop() {
        scanTimer?.cancel()
        scanTimer = nil
        central?.stopScan()
        if let peripheral = peripheral {
            central?.cancelPeripheralConnection(peripheral)
        }
        peripheral = nil
        central = nil
    }

    // MARK: - 命令

    func pair(pin: String) {
        writeJSON(["pin": pin])
    }

    func apply(ssid: String, password: String, host: String, port: Int) {
        // 逐字段下发而不是一次发一个大对象: 每个字段都能单独确认, 失败时也知道
        // 是哪一条没进去。
        writeJSON(["ssid": ssid])
        writeJSON(["pass": password])
        writeJSON(["host": host])
        writeJSON(["port": port])
        writeJSON(["commit": true])
    }

    func forget() {
        writeJSON(["forget": true])
    }

    private func writeJSON(_ object: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else {
            emit(.failed("命令序列化失败"))
            return
        }
        write(text)
    }

    private func write(_ text: String) {
        guard let peripheral = peripheral, let characteristic = cmdCharacteristic else {
            emit(.failed("还没连上设备, 命令没有发出"))
            return
        }

        guard let data = (text + "\n").data(using: .utf8) else { return }
        // MTU 协商出来之前这个值是 0, 兜一个 20(ATT 默认净荷)。
        let chunkSize = max(20, peripheral.maximumWriteValueLength(for: .withResponse))

        var offset = 0
        while offset < data.count {
            let end = min(offset + chunkSize, data.count)
            writeQueue.append((characteristic, data.subdata(in: offset..<end)))
            offset = end
        }
        pumpWrites()
    }

    private func pumpWrites() {
        guard !writing, let peripheral = peripheral, !writeQueue.isEmpty else { return }
        let (characteristic, chunk) = writeQueue.removeFirst()
        writing = true
        peripheral.writeValue(chunk, for: characteristic, type: .withResponse)
    }

    // MARK: - 解析

    private func handleStatus(_ data: Data) {
        statusBuffer.append(data)
        // 设备是"一次通知一个完整 JSON", 但按行切更保险: 万一以后改成多行流,
        // 这里不用动。
        while let newline = statusBuffer.firstIndex(of: 0x0A) {
            let line = statusBuffer[statusBuffer.startIndex..<newline]
            statusBuffer.removeSubrange(statusBuffer.startIndex...newline)
            parseStatus(Data(line))
        }
        if !statusBuffer.isEmpty, let object = decodeJSON(statusBuffer) {
            statusBuffer.removeAll()
            emit(.status(classify(object)))
        }
    }

    /// 解一行状态并投递出去; 解不出来就当成噪声丢掉(设备不会发半行完整 JSON)。
    private func parseStatus(_ data: Data) {
        guard let object = decodeJSON(data) else { return }
        emit(.status(classify(object)))
    }

    private func decodeJSON(_ data: Data) -> [String: Any]? {
        guard !data.isEmpty,
              let object = try? JSONSerialization.jsonObject(with: data),
              let dictionary = object as? [String: Any] else { return nil }
        return dictionary
    }

    private func classify(_ object: [String: Any]) -> ProvisionStatus {
        let name = object["event"] as? String ?? ""
        switch name {
        case "paired":
            return .paired
        case "pin_error":
            return .pinRejected(left: object["left"] as? Int ?? 0)
        case "set":
            return .fieldSet(object["key"] as? String ?? "?")
        case "applying":
            return .applying
        case "done":
            return .done(ip: object["ip"] as? String ?? "?")
        case "failed":
            return .deviceFailed(ProvisionReason.localize(object["message"] as? String ?? "?"))
        case "forgot":
            return .forgot
        case "error":
            return .deviceError(ProvisionReason.localize(object["message"] as? String ?? "?"))
        default:
            return .unknown(name.isEmpty ? "?" : name, object)
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension ProvisionClient: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            emit(.log("开始扫描配网中的设备…"))
            emit(.scanning)
            central.scanForPeripherals(withServices: [ProvisionUUID.service],
                                       options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + scanWindow)
            timer.setEventHandler { [weak self] in self?.connectStrongest() }
            timer.resume()
            scanTimer = timer
        case .poweredOff:
            emit(.failed("蓝牙没打开。打开蓝牙后重试。"))
        case .unauthorized:
            emit(.failed("这个应用没有蓝牙权限。到「系统设置 → 隐私与安全 → 蓝牙」里允许 Codex Pet Bridge。"))
        case .unsupported:
            emit(.failed("这台 Mac 不支持低功耗蓝牙。"))
        default:
            emit(.log("蓝牙状态: \(central.state.rawValue)"))
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        // 广播包里塞的是 128 位 UUID, 名字在扫描响应里; 拿不到就先用短标识兜底。
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? peripheral.name
            ?? "CodexPet-\(peripheral.identifier.uuidString.prefix(4))"
        discovered[peripheral.identifier] = (peripheral, name, RSSI.intValue)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        emit(.log("已连接 \(peripheral.name ?? "设备"), 正在读取服务…"))
        peripheral.discoverServices([ProvisionUUID.service])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral, error: Error?) {
        emit(.failed("连接失败: \(error?.localizedDescription ?? "原因不明")"))
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        // 配网成功后设备会主动收掉蓝牙(省电), 这不是错误。
        let reason = error?.localizedDescription ?? "设备已结束配网"
        emit(.disconnected(reason))
    }
}

// MARK: - CBPeripheralDelegate

extension ProvisionClient: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            emit(.failed("读取服务失败: \(error.localizedDescription)"))
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == ProvisionUUID.service })
        else {
            emit(.failed("这台设备没有配网服务。固件可能还没带蓝牙配网。"))
            return
        }
        peripheral.discoverCharacteristics(
            [ProvisionUUID.cmd, ProvisionUUID.status, ProvisionUUID.info], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error = error {
            emit(.failed("读取特征失败: \(error.localizedDescription)"))
            return
        }
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case ProvisionUUID.cmd: cmdCharacteristic = characteristic
            case ProvisionUUID.status: statusCharacteristic = characteristic
            case ProvisionUUID.info: infoCharacteristic = characteristic
            default: break
            }
        }
        guard cmdCharacteristic != nil, statusCharacteristic != nil else {
            emit(.failed("配网特征不完整, 换个固件版本试试。"))
            return
        }
        peripheral.setNotifyValue(true, for: statusCharacteristic!)
        if let info = infoCharacteristic {
            peripheral.readValue(for: info)
        } else {
            emit(.connected(deviceInfo: [:]))
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error = error {
            emit(.failed("打开状态通知失败: \(error.localizedDescription)"))
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        if characteristic.uuid == ProvisionUUID.info {
            emit(.connected(deviceInfo: decodeJSON(data) ?? [:]))
        } else if characteristic.uuid == ProvisionUUID.status {
            handleStatus(data)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        writing = false
        if let error = error {
            // 一条写失败就清空队列: 后面的 writeJSON 依赖前面那条已经落地
            // (比如先 ssid 再 commit), 半途继续只会给出错误的结果。
            writeQueue.removeAll()
            emit(.failed("写入设备失败: \(error.localizedDescription)"))
            return
        }
        pumpWrites()
    }
}
