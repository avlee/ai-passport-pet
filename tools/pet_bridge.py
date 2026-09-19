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
import glob
import json
import os
import socket
import sys
import threading
import time
from pathlib import Path

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

    def send_state(self, state: str, text: str | None = None) -> bool:
        payload: dict = {"type": "state", "state": state}
        if text:
            payload["text"] = truncate_utf8(squash(text))
        ok = self.send(payload)
        shown = f" / {payload.get('text')}" if payload.get("text") else ""
        print(f"[state] {state}{shown}{'' if ok else '  (设备未连接, 已丢弃)'}",
              flush=True)
        return ok

    def send_text(self, text: str) -> bool:
        return self.send({"type": "text", "text": truncate_utf8(squash(text))})


# ---------------------------------------------------------------------------
# Codex 会话日志跟随
# ---------------------------------------------------------------------------
class CodexWatcher(threading.Thread):
    """跟随最新被改动的 rollout 文件, 把事件映射成宠物状态。"""

    def __init__(self, link: DeviceLink, idle_after: float) -> None:
        super().__init__(daemon=True, name="codex-watcher")
        self.link = link
        self.idle_after = idle_after
        self._current: str | None = None
        self._offset = 0
        self._last_event_at = time.monotonic()
        self._state = "idle"
        # 用户手工下发的状态优先于日志推断, 直到下一个 Codex 事件出现。
        self.manual_until_event = False

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
        self.link.send_state(state, text)

    def _switch(self, path: str) -> None:
        self._current = path
        # 新会话从当前末尾开始跟, 不重放历史 —— 否则一连上就会看到几个月前的
        # 旧事件在屏幕上飞快地刷一遍。
        try:
            self._offset = os.path.getsize(path)
        except OSError:
            self._offset = 0
        print(f"[codex] 跟随会话: {Path(path).name}", flush=True)

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
    """读设备发回来的消息(hello / pong / poke)。"""
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
                    print(f"[recv] hello fw={message.get('fw')} "
                          f"pet={message.get('pet')}", flush=True)
                elif mtype == "poke":
                    print("[recv] 用户戳了宠物一下", flush=True)
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
            if watcher is not None:
                watcher.manual_until_event = True
                watcher._state = head
            link.send_state(head, rest or None)
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
    args = parser.parse_args()

    if args.list_sessions:
        return list_sessions()

    link = DeviceLink()

    watcher: CodexWatcher | None = None
    if not args.no_codex:
        watcher = CodexWatcher(link, args.idle_after)
        watcher.start()
    else:
        print("[codex] 已禁用会话跟随(--no-codex)", flush=True)

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
