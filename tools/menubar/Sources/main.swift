// tools/menubar/Sources/main.swift
//
// 入口。菜单栏应用: 没有窗口, 没有 Dock 图标(Info.plist 里 LSUIElement=1)。
//
// 退出时务必把 pet_bridge.py 子进程一起收掉 —— 否则端口会一直被占着, 下次启动
// 新桥接只会报 "Address already in use"。

import AppKit

// --provision: 无界面的配网模式。放在启动 NSApplication 之前判断 —— 配网要能被
// 脚本调用, 也不该为此在菜单栏里多出一个图标。
var launchArguments = Array(CommandLine.arguments.dropFirst())
if let index = launchArguments.firstIndex(of: "--provision") {
    launchArguments.remove(at: index)
    exit(ProvisionCommand.run(arguments: launchArguments))
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var controller: StatusMenuController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let settings = BridgeSettings.load()
        let controller = StatusMenuController(supervisor: BridgeSupervisor(settings: settings))
        self.controller = controller
        controller.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        controller?.stop()
    }
}

let application = NSApplication.shared
let appDelegate = AppDelegate()
application.delegate = appDelegate
application.setActivationPolicy(.accessory)
application.run()
