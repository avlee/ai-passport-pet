#!/usr/bin/env python3
"""Pet Bridge —— 把 Mac 上的 Codex 状态推给 AI Passport 上的宠物。

架构
----
设备是 TCP 客户端, 本脚本是服务端:

    Codex 会话日志 ──┐
                     ├─> pet_bridge.py ──TCP(JSON 行)──> AI Passport
    键盘/脚本指令 ───┘

设备侧不需要监听端口, 也不依赖 Bonjour/mDNS 发现, 只要知道 Mac 的局域网 IP 即可
(填进固件的 main/pet_config.h)。

状态从哪来
----------
Codex 会把每一轮对话写成 rollout JSONL: ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl
本脚本跟随"最新被改动的那个文件", 把里面的事件映射成宠物状态:

    event_msg/task_started          -> working   开始干活
    event_msg/item_completed        -> working   进度(命令/思考/改文件…)
    event_msg/task_complete         -> ready     本轮完成
    error 字段非空 / error 事件      -> failed    出错
    一段时间没有任何事件             -> idle      空闲
    审批/确认类事件                  -> waiting   等你确认

无法从日志判断状态时, 也可以直接在终端敲命令手工驱动(见 --help)。

用法
----
    python3 tools/pet_bridge.py                  # 跟随最新 Codex 会话 + 可交互
    python3 tools/pet_bridge.py --no-codex       # 只监听, 状态全靠手工命令
    python3 tools/pet_bridge.py --port 8765 --bind 0.0.0.0
    python3 tools/pet_bridge.py --list-sessions  # 看看会跟哪个文件

控制通道(--control)
-------------------
给 GUI 前端(macOS 菜单栏应用)用的本地通道: AF_UNIX + JSON 行, 双向。

    python3 tools/pet_bridge.py --control "$HOME/Library/Application Support/CodexPetBridge/bridge.sock"

连上来先收到一条 snapshot(全部当前状态), 之后是增量事件:

    事件(服务端 -> 客户端)          字段
    bridge   桥接自身            running, port, bind, pid
    link     设备连接            connected, peer
    state    Codex 状态          state, text, source
    text     只换气泡文字        text
    session  跟随的会话文件      name, path
    watch    是否跟随日志        enabled
    device   设备上报            kind(hello/poke), fw, pet
    battery  设备电量            soc(0..100, null 表示读不到)
    pong     ping 的回应
    error    命令出错            message

    命令(客户端 -> 服务端)
    {"cmd":"state","state":"working","text":"..."}   手工推状态
    {"cmd":"text","text":"..."}                      只换气泡文字
    {"cmd":"raw","payload":{...}}                    直接发一行 JSON
    {"cmd":"watch","enabled":false}                  开关日志跟随
    {"cmd":"snapshot"}                               重发一次全量状态
    {"cmd":"ping"}

    注意: 这是一个**事件流**, 不是一问一答。命令成功不回执 —— 结果会以对应的事件
    广播出来(state/text/watch/...); 只有出错才会多收到一条 error。客户端按事件
    更新状态即可, 不要去配"发一条收一条"。

控制通道只是**观察和驱动**上的补充, 协议的唯一定义仍然是本文件里发往设备的
那几条 JSON —— 所以 tests/test_pet_bridge.py 的两端互校依然覆盖全部下行报文。

交互命令(启动后在同一个终端里直接敲):
    working 正在重构登录模块
    text 只改文字不改状态
    waiting / ready / failed / idle
    raw {"type":"state","state":"jumping"}
    status
    quit
"""

from __future__ import annotations

import argparse
import contextlib
import glob
import json
import os
import socket
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable

# ---------------------------------------------------------------------------
# 协议常量。必须与 main/pet_protocol.h 保持一致。
# ---------------------------------------------------------------------------
DEFAULT_PORT = 8765
DEFAULT_BIND = "0.0.0.0"
# 设备侧 PET_PROTOCOL_TEXT_MAX = 192 字节(含结尾 NUL)。按 UTF-8 字节数截断,
# 保证不会把多字节字符切一半。
TEXT_MAX_BYTES = 180
# 设备侧 PET_BRIDGE_IDLE_TIMEOUT_MS = 12000; 心跳要明显快于它。
PING_INTERVAL_S = 5.0
# 多久没有 Codex 事件就把宠物切回 idle。
CODEX_IDLE_AFTER_S = 45.0
CODEX_SESSIONS_GLOB = os.path.expanduser("~/.codex/sessions/**/rollout-*.jsonl")

VALID_STATES = ("idle", "working", "waiting", "ready", "failed")


def truncate_utf8(text: str, max_bytes: int = TEXT_MAX_BYTES) -> str:
    """按 UTF-8 字节数截断, 不切开多字节字符, 超长时补省略号。"""
    encoded = text.encode("utf-8")
    if len(encoded) <= max_bytes:
        return text
    # 省略号本身也要占字节预算。
    budget = max_bytes - len("…".encode("utf-8"))
    cut = encoded[:budget]
    # 退到字符边界: 丢掉尾部不完整的续字节。
    while cut and (cut[-1] & 0xC0) == 0x80:
        cut = cut[:-1]
    return cut.decode("utf-8", errors="ignore") + "…"


def squash(text: str) -> str:
    """把多行/连续空白压成单行, 屏幕上的一行气泡放不下多行文本。"""
    return " ".join(text.split())


class DeviceLink:
    """与设备的一条 TCP 连接。写操作加锁, 多个状态来源可以安全并发调用。"""

    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._lock = threading.Lock()
        self._peer = ""
        # 状态广播出口。默认没人接, 所以单测里直接 DeviceLink() 就行, 不必造 hub。
        self.on_event: Callable[[dict], None] | None = None

    def _publish(self, event: dict) -> None:
        if self.on_event is not None:
            self.on_event(event)

    @property
    def connected(self) -> bool:
        return self._sock is not None

    @property
    def peer(self) -> str:
        return self._peer

    def attach(self, sock: socket.socket, peer: str) -> None:
        with self._lock:
            self._sock = sock
            self._peer = peer
        print(f"[link] 设备已连接: {peer}", flush=True)
        self._publish({"event": "link", "connected": True, "peer": peer})

    def detach(self) -> None:
        with self._lock:
            sock, self._sock = self._sock, None
            self._peer = ""
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
            print("[link] 设备已断开, 宠物会进入睡眠", flush=True)
            self._publish({"event": "link", "connected": False, "peer": ""})

    def send(self, payload: dict) -> bool:
        line = (json.dumps(payload, ensure_ascii=False) + "\r\n").encode("utf-8")
        with self._lock:
            sock = self._sock
            if sock is None:
                return False
            try:
                sock.sendall(line)
                return True
            except OSError as exc:
                print(f"[link] 发送失败({exc}), 断开连接", flush=True)
        # 走到这里说明写失败, 在锁外收尾避免重入。
        self.detach()
        return False

    def send_state(self, state: str, text: str | None = None,
                   source: str = "codex") -> bool:
        payload: dict = {"type": "state", "state": state}
        if text:
            payload["text"] = truncate_utf8(squash(text))
        ok = self.send(payload)
        shown = f" / {payload.get('text')}" if payload.get("text") else ""
        print(f"[state] {state}{shown}{'' if ok else '  (设备未连接, 已丢弃)'}",
              flush=True)
        self._publish({
            "event": "state",
            "state": state,
            "text": payload.get("text", ""),
            "source": source,
            "delivered": ok,
        })
        return ok

    def send_text(self, text: str) -> bool:
        squashed = truncate_utf8(squash(text))
        ok = self.send({"type": "text", "text": squashed})
        # 单独一种事件名: 只换文字不该把最近一次的 state 从 snapshot 里顶掉。
        self._publish({"event": "text", "text": squashed, "delivered": ok})
        return ok


# ---------------------------------------------------------------------------
# Codex 会话日志跟随
# ---------------------------------------------------------------------------
class CodexWatcher(threading.Thread):
    """跟随最新被改动的 rollout 文件, 把事件映射成宠物状态。"""

    def __init__(self, link: DeviceLink, idle_after: float,
                 on_event: Callable[[dict], None] | None = None) -> None:
        super().__init__(daemon=True, name="codex-watcher")
        self.link = link
        self.idle_after = idle_after
        self.on_event = on_event
        self._current: str | None = None
        self._offset = 0
        self._last_event_at = time.monotonic()
        self._state = "idle"
        # 用户手工下发的状态优先于日志推断, 直到下一个 Codex 事件出现。
        self.manual_until_event = False
        # 菜单栏可以临时停掉日志跟随(演示时只想手工切状态)。
        self.enabled = True

    def _publish(self, event: dict) -> None:
        if self.on_event is not None:
            self.on_event(event)

    @property
    def state(self) -> str:
        return self._state

    @property
    def session_name(self) -> str:
        return Path(self._current).name if self._current else ""

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = bool(enabled)
        self._publish({"event": "watch", "enabled": self.enabled})

    @staticmethod
    def newest_session() -> str | None:
        files = glob.glob(CODEX_SESSIONS_GLOB, recursive=True)
        if not files:
            return None
        return max(files, key=os.path.getmtime)

    def _emit(self, state: str, text: str | None) -> None:
        self._state = state
        self._last_event_at = time.monotonic()
        self.manual_until_event = False
        self.link.send_state(state, text, source="codex")

    def _switch(self, path: str) -> None:
        self._current = path
        # 新会话从当前末尾开始跟, 不重放历史 —— 否则一连上就会看到几个月前的
        # 旧事件在屏幕上飞快地刷一遍。
        try:
            self._offset = os.path.getsize(path)
        except OSError:
            self._offset = 0
        print(f"[codex] 跟随会话: {Path(path).name}", flush=True)
        self._publish({"event": "session", "name": Path(path).name, "path": path})

    def _handle(self, record: dict) -> None:
        rtype = record.get("type")
        payload = record.get("payload")
        payload = payload if isinstance(payload, dict) else {}
        ptype = payload.get("type")

        if payload.get("error"):
            self._emit("failed", str(payload["error"]))
            return

        if rtype == "event_msg" and ptype == "task_started":
            self._emit("working", "Codex 开始处理任务")
            return

        if rtype == "event_msg" and ptype == "task_complete":
            self._emit("ready", str(payload.get("last_agent_message") or ""))
            return

        if rtype == "event_msg" and ptype == "item_completed":
            item = payload.get("item")
            item = item if isinstance(item, dict) else {}
            itype = item.get("type")
            if itype == "CommandExecution":
                content = item.get("content")
                text = content if isinstance(content, str) else ""
                self._emit("working", text or "正在执行命令")
            elif itype == "Reasoning":
                self._emit("working", "正在思考…")
            elif itype == "FileChange":
                self._emit("working", "正在修改文件")
            elif itype == "AgentMessage":
                self._emit("working", str(item.get("content") or ""))
            elif itype == "UserMessage":
                # 新的用户消息: 下一轮通常马上开始。
                self._emit("working", "收到新指令")
            else:
                self._emit("working", str(itype or "工作中"))
            return

        # 等用户确认 / 授权: 具体事件名随版本变化, 用关键词兜底匹配。
        name = f"{rtype}/{ptype}".lower()
        if any(word in name for word in ("approval", "elicit", "confirm", "input_request")):
            self._emit("waiting", "等待你确认")
            return

    def run(self) -> None:
        while True:
            if not self.enabled:
                time.sleep(0.25)
                continue
            try:
                newest = self.newest_session()
                if newest is None:
                    time.sleep(1.0)
                    continue
                if newest != self._current:
                    self._switch(newest)

                assert self._current is not None
                # 文件被截断/轮转时重新定位到开头。
                if os.path.getsize(self._current) < self._offset:
                    self._offset = 0

                with open(self._current, "rb") as handle:
                    handle.seek(self._offset)
                    for raw in handle:
                        self._offset += len(raw)
                        line = raw.strip()
                        if not line:
                            continue
                        try:
                            record = json.loads(line.decode("utf-8", "replace"))
                        except json.JSONDecodeError:
                            continue
                        self._handle(record)

                if (self._state != "idle"
                        and time.monotonic() - self._last_event_at > self.idle_after):
                    self._emit("idle", "")
            except OSError as exc:
                print(f"[codex] 读日志出错: {exc}", flush=True)
            time.sleep(0.25)


# ---------------------------------------------------------------------------
# 服务端
# ---------------------------------------------------------------------------
def serve(link: DeviceLink, bind: str, port: int) -> None:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((bind, port))
    server.listen(1)
    print(f"[serv] 监听 {bind}:{port}, 等待设备连接…", flush=True)
    if bind == DEFAULT_BIND:
        try:
            import subprocess
            # 提示一下该往固件里填哪个地址, 省得去查 ifconfig。
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.connect(("192.168.1.1", 1))
            local_ip = probe.getsockname()[0]
            probe.close()
            print(f"[serv] 局域网地址看起来是 {local_ip} "
                  f"(固件里的 PET_BRIDGE_HOST 填这个)", flush=True)
        except OSError:
            pass

    while True:
        conn, addr = server.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # 同一时间只服务一台设备; 新连接顶掉旧的。
        link.detach()
        link.attach(conn, f"{addr[0]}:{addr[1]}")

        threading.Thread(target=read_device, args=(link, conn),
                         daemon=True, name="device-reader").start()
        threading.Thread(target=ping_loop, args=(link,),
                         daemon=True, name="keepalive").start()


def read_device(link: DeviceLink, conn: socket.socket) -> None:
    """读设备发回来的消息(hello / battery / poke / pong)。"""
    buffer = b""
    try:
        while True:
            chunk = conn.recv(512)
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    message = json.loads(line.decode("utf-8", "replace"))
                except json.JSONDecodeError:
                    print(f"[recv] 非 JSON: {line!r}", flush=True)
                    continue
                mtype = message.get("type")
                if mtype == "hello":
                    fw = message.get("fw") or ""
                    pet = message.get("pet") or ""
                    print(f"[recv] hello fw={fw} pet={pet}", flush=True)
                    # 一并报给控制通道: 菜单栏要显示设备的固件版本与宠物包。
                    # (_publish 是本模块内部的出口, read_device 与 DeviceLink 同模块。)
                    link._publish({"event": "device", "kind": "hello",
                                   "fw": fw, "pet": pet})
                elif mtype == "poke":
                    print("[recv] 用户戳了宠物一下", flush=True)
                    link._publish({"event": "device", "kind": "poke"})
                elif mtype == "battery":
                    # 单独一个事件名, 不并进 device: ControlHub 每类只留最新一份,
                    # 挤在 device 里会把 hello 报上来的固件/宠物包顶掉。
                    soc = message.get("soc")
                    if not isinstance(soc, int) or not 0 <= soc <= 100:
                        soc = None
                    print(f"[recv] 电量 {soc if soc is not None else '未知'}", flush=True)
                    link._publish({"event": "battery", "soc": soc})
                elif mtype != "pong":
                    print(f"[recv] {message}", flush=True)
    except OSError:
        pass
    finally:
        link.detach()


def ping_loop(link: DeviceLink) -> None:
    """心跳: 设备超过 12 秒收不到任何消息就会判定掉线并让宠物睡觉。"""
    while link.connected:
        time.sleep(PING_INTERVAL_S)
        if link.connected:
            link.send({"type": "ping"})


# ---------------------------------------------------------------------------
# 控制通道(本地 GUI 前端用)
# ---------------------------------------------------------------------------
# 它只是**观察与驱动**层: 所有发往设备的报文仍然走 DeviceLink, 这里把同一份状态
# 再广播给本地前端, 并把前端命令翻译成和终端 REPL 完全相同的调用 —— 这样终端和
# GUI 的行为不会分叉, 两端的报文也仍然由同一份实现产生。
CONTROL_PROTOCOL = 1


class ControlHub:
    """AF_UNIX + JSON 行, 双向: 状态往外推, 命令往里收。"""

    def __init__(self, path: str,
                 on_command: Callable[[dict], dict | None] | None = None) -> None:
        self.path = path
        self.on_command = on_command
        self._server: socket.socket | None = None
        self._clients: list[socket.socket] = []
        self._lock = threading.Lock()
        # 各类事件的最新一份。新客户端接入时用它拼出 snapshot, 所以 publish 可以
        # 早于 start —— 应用连上来就能一次拿全当前状态, 不必等下一个事件。
        self._latest: dict[str, dict] = {}

    def publish(self, event: dict) -> None:
        name = event.get("event")
        if name and name not in ("pong", "error"):
            with self._lock:
                self._latest[name] = event
        line = (json.dumps(event, ensure_ascii=False) + "\n").encode("utf-8")
        with self._lock:
            clients = list(self._clients)
        for client in clients:
            try:
                client.sendall(line)
            except OSError:
                self._drop(client)

    def snapshot(self) -> dict:
        with self._lock:
            latest = dict(self._latest)
        return {"event": "snapshot", "protocol": CONTROL_PROTOCOL, "state": latest}

    @property
    def client_count(self) -> int:
        with self._lock:
            return len(self._clients)

    def _drop(self, client: socket.socket) -> None:
        with self._lock:
            if client in self._clients:
                self._clients.remove(client)
        with contextlib.suppress(OSError):
            client.close()

    def start(self) -> None:
        parent = os.path.dirname(self.path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        # 上次没退干净会留下 socket 文件, bind 前先清掉。
        with contextlib.suppress(OSError):
            os.unlink(self.path)
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(self.path)
        server.listen(4)
        self._server = server
        threading.Thread(target=self._accept_loop, daemon=True,
                         name="control-accept").start()
        print(f"[ctrl] 控制通道就绪: {self.path}", flush=True)

    def _accept_loop(self) -> None:
        assert self._server is not None
        while True:
            try:
                client, _ = self._server.accept()
            except OSError:
                return
            with self._lock:
                self._clients.append(client)
            self._send(client, self.snapshot())
            threading.Thread(target=self._read_loop, args=(client,), daemon=True,
                             name="control-client").start()

    def _read_loop(self, client: socket.socket) -> None:
        buffer = b""
        try:
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    return
                buffer += chunk
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    line = line.strip()
                    if line:
                        self._dispatch(client, line)
        except OSError:
            pass
        finally:
            self._drop(client)

    def _dispatch(self, client: socket.socket, line: bytes) -> None:
        try:
            command = json.loads(line.decode("utf-8", "replace"))
        except json.JSONDecodeError:
            self._send(client, {"event": "error", "message": "命令不是合法 JSON"})
            return
        if not isinstance(command, dict):
            self._send(client, {"event": "error", "message": "命令必须是 JSON 对象"})
            return

        name = command.get("cmd")
        if name == "ping":
            self._send(client, {"event": "pong"})
            return
        if name == "snapshot":
            self._send(client, self.snapshot())
            return
        if self.on_command is None:
            self._send(client, {"event": "error", "message": "未接入命令处理器"})
            return

        reply = self.on_command(command)
        if reply is not None:
            self._send(client, reply)

    @staticmethod
    def _send(client: socket.socket, event: dict) -> None:
        with contextlib.suppress(OSError):
            client.sendall((json.dumps(event, ensure_ascii=False) + "\n").encode("utf-8"))


def build_command_router(link: DeviceLink, watcher: "CodexWatcher | None"):
    """把控制通道的命令翻译成与终端 REPL 相同的调用。

    成功返回 None(结果由事件广播体现), 失败返回一条 error 事件。刻意不做逐条回执:
    回执和广播会交错到同一条流里, 客户端很容易把广播当成上一条命令的回执。
    """

    def handle(command: dict) -> dict | None:
        name = command.get("cmd")

        if name == "state":
            state = str(command.get("state", ""))
            if state not in VALID_STATES:
                return {"event": "error", "message": f"未知状态: {state}"}
            text = command.get("text")
            apply_state(link, watcher, state, text if isinstance(text, str) else None)
            return None

        if name == "text":
            text = command.get("text")
            if not isinstance(text, str):
                return {"event": "error", "message": "text 必须是字符串"}
            link.send_text(text)
            return None

        if name == "raw":
            payload = command.get("payload")
            if not isinstance(payload, dict):
                return {"event": "error", "message": "payload 必须是 JSON 对象"}
            link.send(payload)
            return None

        if name == "watch":
            if watcher is None:
                return {"event": "error", "message": "启动时用了 --no-codex, 无法开启跟随"}
            watcher.set_enabled(bool(command.get("enabled")))
            return None

        return {"event": "error", "message": f"未知命令: {name}"}

    return handle


# ---------------------------------------------------------------------------
# 手工命令
# ---------------------------------------------------------------------------
HELP = """可用命令:
  <state> [文字]      推一个状态; state ∈ idle/working/waiting/ready/failed
  text <文字>         只更新气泡文字, 不改状态
  raw <JSON>          直接发一行 JSON
  status              打印当前连接与状态
  quit                退出
示例:
  working 正在重构登录模块
  ready 改完了, 跑一下测试
  text 只是换一句话
"""


def apply_state(link: DeviceLink, watcher: CodexWatcher | None,
                state: str, text: str | None) -> None:
    """手工推一个状态。终端 REPL 与控制通道共用, 保证两边语义一致。

    手工状态优先于日志推断, 直到下一个 Codex 事件出现 —— 否则刚切过去就会被
    日志里的旧事件覆盖回去。
    """
    if watcher is not None:
        watcher.manual_until_event = True
        watcher._state = state
    link.send_state(state, text or None, source="manual")


def repl(link: DeviceLink, watcher: CodexWatcher | None) -> None:
    print(HELP, flush=True)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if line in ("quit", "exit", "q"):
            print("[exit] 再见", flush=True)
            os._exit(0)
        if line in ("help", "?"):
            print(HELP, flush=True)
            continue
        if line == "status":
            state = watcher._state if watcher else "-"
            print(f"[status] 设备={link.peer or '未连接'} codex状态={state}",
                  flush=True)
            continue

        head, _, rest = line.partition(" ")
        rest = rest.strip()

        if head == "text":
            link.send_text(rest)
            continue
        if head == "raw":
            try:
                link.send(json.loads(rest))
            except json.JSONDecodeError as exc:
                print(f"[err] JSON 解析失败: {exc}", flush=True)
            continue
        if head in VALID_STATES:
            apply_state(link, watcher, head, rest or None)
            continue

        print(f"[err] 不认识: {head!r}; 敲 help 看用法", flush=True)


def list_sessions() -> int:
    files = glob.glob(CODEX_SESSIONS_GLOB, recursive=True)
    if not files:
        print(f"没有找到会话文件: {CODEX_SESSIONS_GLOB}", file=sys.stderr)
        return 1
    files.sort(key=os.path.getmtime, reverse=True)
    print(f"找到 {len(files)} 个会话文件, 按最近修改排序:")
    for path in files[:10]:
        stamp = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(path)))
        print(f"  {stamp}  {path}")
    print(f"\n将跟随: {files[0]}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把 Codex 状态推给 AI Passport 上的宠物",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=HELP,
    )
    parser.add_argument("--bind", default=DEFAULT_BIND, help="监听地址")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="监听端口")
    parser.add_argument("--no-codex", action="store_true",
                        help="不跟随 Codex 会话日志, 状态只靠手工命令")
    parser.add_argument("--idle-after", type=float, default=CODEX_IDLE_AFTER_S,
                        help=f"多久没有 Codex 事件就切回 idle(秒), 默认 "
                             f"{CODEX_IDLE_AFTER_S:g}")
    parser.add_argument("--list-sessions", action="store_true",
                        help="列出会跟随的 Codex 会话文件后退出")
    parser.add_argument("--control", metavar="SOCKET",
                        help="额外开一个本地控制通道(AF_UNIX socket 路径), "
                             "供菜单栏/GUI 读取状态并下发命令")
    args = parser.parse_args()

    if args.list_sessions:
        return list_sessions()

    hub: ControlHub | None = None
    if args.control:
        hub = ControlHub(os.path.expanduser(args.control))

    link = DeviceLink()
    if hub is not None:
        link.on_event = hub.publish
        # 先发布一次初始状态, 这样前端连上来拿到的 snapshot 就是完整的, 不用等事件。
        hub.publish({"event": "bridge", "running": True, "pid": os.getpid(),
                     "bind": args.bind, "port": args.port,
                     "codex": not args.no_codex})
        hub.publish({"event": "link", "connected": False, "peer": ""})

    watcher: CodexWatcher | None = None
    if not args.no_codex:
        watcher = CodexWatcher(link, args.idle_after,
                               on_event=hub.publish if hub else None)
        watcher.start()
    else:
        print("[codex] 已禁用会话跟随(--no-codex)", flush=True)

    if hub is not None:
        hub.publish({"event": "watch", "enabled": watcher is not None})
        hub.on_command = build_command_router(link, watcher)
        try:
            hub.start()
        except OSError as exc:
            # 控制通道只是附加能力, 打不开不该拖垮主服务(设备还得照常连)。
            print(f"[ctrl] 控制通道打开失败({exc}), 仅以终端模式运行", flush=True)
            link.on_event = None
            hub = None

    threading.Thread(target=serve, args=(link, args.bind, args.port),
                     daemon=True, name="server").start()

    if sys.stdin.isatty():
        try:
            repl(link, watcher)
        except KeyboardInterrupt:
            pass
    else:
        # 非交互场景(被脚本拉起): 只跑服务端。
        print("[repl] stdin 不是终端, 跳过交互命令", flush=True)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
