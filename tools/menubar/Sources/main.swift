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
        installEditMenu()
        let settings = BridgeSettings.load()
        let controller = StatusMenuController(supervisor: BridgeSupervisor(settings: settings))
        self.controller = controller
        controller.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        controller?.stop()
    }

    /// LSUIElement 应用没有菜单栏, 也就没有承载 Cmd+X/C/V/A/Z 的编辑菜单 —— 而这些
    /// 快捷键是靠主菜单分发进 responder chain 的, 没有菜单, 所有文本框里的粘贴/剪切
    /// 全部失灵(配网窗口和气泡输入框都中过招)。装一套标准编辑菜单: 菜单栏照样不显示
    /// (没有 Dock 图标), 但快捷键活了。Swift 版 NSUndoManager 的 action 名是 undo:/redo:。
    private func installEditMenu() {
        let edit = NSMenu(title: "编辑")
        edit.addItem(NSMenuItem(title: "撤销", action: Selector(("undo:")), keyEquivalent: "z"))
        edit.addItem(NSMenuItem(title: "重做", action: Selector(("redo:")), keyEquivalent: "Z"))
        edit.addItem(.separator())
        for (title, action, key) in [("剪切", "cut:", "x"), ("拷贝", "copy:", "c"),
                                     ("粘贴", "paste:", "v"), ("全选", "selectAll:", "a")] {
            let item = NSMenuItem(title: title, action: Selector(action), keyEquivalent: key)
            edit.addItem(item)
        }
        let main = NSMenu()
        let editItem = NSMenuItem()
        editItem.title = "编辑"
        editItem.submenu = edit
        main.addItem(editItem)
        NSApp.mainMenu = main
    }
}

let application = NSApplication.shared
let appDelegate = AppDelegate()
application.delegate = appDelegate
application.setActivationPolicy(.accessory)
application.run()
