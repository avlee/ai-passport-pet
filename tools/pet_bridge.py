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
    pets     可选宠物列表        pets_dir, cache_dir, slot_bytes, packer, items[],
                                 busy, last
                                 packer.available 为假表示这台桥接缺 Pillow,
                                 打不出 .pet —— 换宠物之前就该告诉用户
    petxfer  换宠物进度          id, state(preparing/sending/sent/failed),
                                 sent, total, message
    petdone  设备确认结果        id(槽里**真正**装上的那只), ok
    pong     ping 的回应
    error    命令出错            message

    命令(客户端 -> 服务端)
    {"cmd":"state","state":"working","text":"..."}   手工推状态
    {"cmd":"text","text":"..."}                      只换气泡文字
    {"cmd":"raw","payload":{...}}                    直接发一行 JSON
    {"cmd":"watch","enabled":false}                  开关日志跟随
    {"cmd":"petlist"}                                重新扫描并广播 pets
    {"cmd":"pet","id":"sophie-portrait"}             换宠物(后台传输, 立刻返回)
    {"cmd":"snapshot"}                               重发一次全量状态
    {"cmd":"ping"}

    注意: 这是一个**事件流**, 不是一问一答。命令成功不回执 —— 结果会以对应的事件
    广播出来(state/text/watch/pets/petxfer/...); 只有出错才会多收到一条 error。
    客户端按事件更新状态即可, 不要去配"发一条收一条"。

    换宠物的时序: 发 {"cmd":"pet"} 之后, 先是若干条 petxfer(sending, 带 sent/total
    进度), 发完一条 petxfer(sent), 然后等设备的 petdone(ok)。**petdone 才是结论** ——
    设备要核对长度与整包 CRC 再重新映射, "发完了"和"装上了"是两件事。成功时设备
    还会补发一条 hello, 菜单栏以它为准更新"当前是哪只"。

    结论一到, 桥接会补发一条 pets: last 收成终局(done/failed), items 里的 cached
    也重扫过了。同时 petxfer 会从 snapshot 里撤掉 —— 它是进度不是状态, 留着最后一帧
    "sent", 中途接入的客户端重放快照时会把终点状态盖回去。

控制通道只是**观察和驱动**上的补充, 协议的唯一定义仍然是本文件里发往设备的
那几条 JSON —— 所以 tests/test_pet_bridge.py 的两端互校依然覆盖全部下行报文。

交互命令(启动后在同一个终端里直接敲):
    working 正在重构登录模块
    text 只改文字不改状态
    waiting / ready / failed / idle
    pets
    pet sophie-portrait
    raw {"type":"state","state":"jumping"}
    status
    quit
"""

from __future__ import annotations

import argparse
import contextlib
import functools
import glob
import json
import os
import socket
import sys
import threading
import time
import zlib
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
# 找"最近在写的会话"时只看最近几个日期目录, 见 `CodexWatcher._recent_rollouts()`。
CODEX_RECENT_MONTHS = 2
CODEX_RECENT_DAYS = 5

# 推送宠物包时的分片大小。只影响进度事件的密度(约 200 次一条 3 MB 的包)。
PET_SEND_CHUNK = 16384

# Codex 的宠物源目录与设备侧槽的容量上限(partitions.csv 里 pets 分区 = 0x3f0000)。
PETS_DIR = Path(os.path.expanduser("~/.codex/pets"))
PET_SLOT_BYTES = 0x3F0000

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


def message_text(value: object) -> str:
    """把 Codex 的字段压成可以上屏的纯文本。

    Codex 不把正文直接放在字段里, 而是包成内容块或结构体: 一条 AgentMessage 的
    `content` 是 `[{"type": "Text", "text": "…"}]`, 报错是
    `{"message": "…", "codex_error_info": …}`。旧代码对这两处都是 `str()` 硬转,
    屏幕上于是出现一段"JSON 字符串" —— 那串 `[{'type': 'Text', 'text': '…'}]`
    就是这么来的。

    2026-09-20 在本机 598 个会话里实测: 829 条 AgentMessage 的 `content` 全是
    列表, 20 条报错全是字典, **一条字符串都没有** —— 也就是说这两个分支从来没
    显示过正文, 一直是把结构体 repr 出去。

    形状对不上就返回空串, 由调用方按状态给兜底文案。**这里绝不 `str()` 兜底**:
    把结构体 str 出去正是这个缺陷本身, 换个没见过的形状也还是同一个病。
    """
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        # 内容块是 `{"type": …, "text": …}`, 报错体是 `{"message": …}`。
        for key in ("text", "message"):
            text = value.get(key)
            if isinstance(text, str):
                return text
        return ""
    if isinstance(value, (list, tuple)):
        # 递归下去: 块本身可能是字典, 也可能(将来)是裸字符串; 多块按顺序拼一行。
        return " ".join(part for part in (message_text(block) for block in value)
                        if part)
    return ""


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

    def send_pet(self, pet_id: str, package: bytes) -> bool:
        """把一份宠物包推给设备: 先一行宣告, 紧跟 size 个**裸字节**。

        为什么不套 base64: 一只宠物 1~3.4 MB, 编码要多传 33%, 而这一步本来就要
        几十秒(设备边收边擦 flash, 是整条链路上最慢的一环)。设备侧也是按裸字节
        流式写进分区的, 没有多余的 RAM 去放一份完整的副本。

        整个过程必须在 `_lock` 里发完 —— 设备收到宣告之后就切进"二进制模式"了,
        这时要是插进去一条 ping 或 state, 那几个字节会被当成包体写进 flash, 整包
        从此错位。代价是传输期间其他发送方会阻塞(心跳延后发, 无害); 这是正确的
        取舍, 不要为了"不卡心跳"把宣告和载荷拆开锁。
        """
        announce = json.dumps(
            {"type": "pet", "id": pet_id, "size": len(package),
             "crc32": zlib.crc32(package) & 0xFFFFFFFF},
            ensure_ascii=False,
        ).encode("utf-8") + b"\r\n"

        total = len(package)
        with self._lock:
            sock = self._sock
            if sock is None:
                return False
            try:
                sock.sendall(announce)
                view = memoryview(package)
                sent = 0
                while sent < total:
                    chunk = view[sent:sent + PET_SEND_CHUNK]
                    sock.sendall(chunk)
                    sent += len(chunk)
                    # 分片上报进度: 整包丢给 sendall 的话, 界面上几十秒都不会动一下。
                    self._publish({"event": "petxfer", "id": pet_id, "state": "sending",
                                   "sent": sent, "total": total})
                return True
            except OSError as exc:
                print(f"[pet] 发送失败({exc}), 断开连接", flush=True)

        # 走到这里说明写失败, 在锁外收尾避免重入。
        self.detach()
        return False


# ---------------------------------------------------------------------------
# 宠物库: 扫描 Codex 的宠物目录, 按需生成设备能收的 .pet
# ---------------------------------------------------------------------------
# 设备那条 3.94 MB 的 pets 分区只装得下一只, 所以这台 Mac 上的"宠物列表"就是
# ~/.codex/pets 下的各个目录 —— 换宠物 = 把另一只重新打包推下去。
#
# 打包用 tools/gen_pet_package.py 的同一份实现(不重写一份): 包格式是跨语言的
# 契约, 只能有一个源头。它需要 Pillow, 所以是惰性导入 —— 没装 Pillow 时"列表"
# 照常能看, 只是装不了, 报错也说得清楚。
DEFAULT_PET_CACHE = (
    os.path.expanduser("~/Library/Application Support/CodexPetBridge/pets")
    if sys.platform == "darwin"
    else os.path.expanduser("~/.cache/codex-pet-bridge/pets")
)


class PetLibrary:
    """~/.codex/pets 的目录视图 + .pet 的按需生成与缓存。

    缓存键是"源图集的大小与改动时间" —— 只要图集没动就不再重新切帧(一只 3 MB
    的宠物要切 57 帧、逐像素转 RGB565, 每次点菜单都做一遍太浪费)。
    """

    def __init__(self, pets_dir: Path = PETS_DIR, cache_dir: Path | None = None) -> None:
        self.pets_dir = Path(pets_dir)
        self.cache_dir = Path(cache_dir) if cache_dir else Path(DEFAULT_PET_CACHE)
        self._lock = threading.Lock()
        self._index: dict[str, dict] = {}

    # -- 扫描 ---------------------------------------------------------------
    def scan(self) -> list[dict]:
        """列出可用的宠物。只读 pet.json, 不切帧 —— 菜单要立刻能弹出来。"""
        items: list[dict] = []
        again: dict[str, dict] = {}

        for entry in sorted(self.pets_dir.iterdir()) if self.pets_dir.is_dir() else []:
            if not entry.is_dir() or not (entry / "pet.json").is_file():
                continue
            try:
                with (entry / "pet.json").open(encoding="utf-8") as handle:
                    manifest = json.load(handle)
            except (OSError, json.JSONDecodeError) as exc:
                items.append({"id": entry.name, "display": entry.name, "source": str(entry),
                              "error": f"pet.json 读不出来: {exc}"})
                continue

            pet_id = str(manifest.get("id") or entry.name)
            item = {
                "id": pet_id,
                "display": str(manifest.get("displayName") or pet_id),
                "source": str(entry),
                "version": manifest.get("spriteVersionNumber"),
            }
            if item["version"] != 2:
                item["error"] = f"只支持 spriteVersionNumber=2, 这里是 {item['version']!r}"
            stamp = self._stamp(entry, manifest)
            cached = self.cache_dir / f"{pet_id}.pet"
            item["stamp"] = stamp
            item["cached"] = bool(stamp) and cached.is_file() and \
                self._cached_stamp(pet_id) == stamp
            if item["cached"]:
                item["size"] = cached.stat().st_size
            items.append(item)
            again[pet_id] = item

        with self._lock:
            self._index = again
        return items

    def entries(self) -> list[dict]:
        """最近一次 scan() 的结果(没扫过就扫一遍)。"""
        if not self._index:
            return self.scan()
        return list(self._index.values())

    @staticmethod
    def _stamp(entry: Path, manifest: dict) -> str:
        sheet = entry / str(manifest.get("spritesheetPath", "spritesheet.webp"))
        try:
            stat = sheet.stat()
        except OSError:
            return ""
        return f"{sheet.name}:{stat.st_size}:{int(stat.st_mtime)}"

    def _cached_stamp(self, pet_id: str) -> str:
        sidecar = self.cache_dir / f"{pet_id}.stamp"
        try:
            return sidecar.read_text(encoding="utf-8").strip()
        except OSError:
            return ""

    # -- 打包能力 -----------------------------------------------------------
    @staticmethod
    def _pil_version() -> str | None:
        """返回当前解释器的 Pillow 版本, 没有就返回 None。"""
        try:
            import PIL  # noqa: F401  只用来问一句"在不在"
        except ImportError:
            return None
        return str(getattr(PIL, "__version__", "?"))

    def packer(self) -> dict:
        """报给菜单: 这台桥接能不能把图集打成 .pet。

        让菜单在**点之前**就知道"点了也会失败", 而不是点完看一句报错 ——
        解释器挑错(比如 GUI 启动时 PATH 里只有 /usr/bin/python3)是很常见的坑,
        从"换宠物失败"这个现象很难反推回去。
        """
        version = self._pil_version()
        return {
            "available": version is not None,
            "python": sys.executable,
            "pillow": version,
            "hint": None if version else
                    f"当前 python({sys.executable})没有 Pillow, 打不出 .pet",
        }

    # -- 生成 ---------------------------------------------------------------
    def package(self, pet_id: str) -> tuple[bytes, dict]:
        """取这只宠物的 .pet 字节与元信息, 必要时先生成。

        返回 (bytes, {"id","display","total","stage"})。失败抛 RuntimeError,
        消息是可以直接显示给用户的中文。
        """
        entry = None
        for candidate in self.entries():
            if candidate["id"] == pet_id:
                entry = candidate
                break
        if entry is None:
            raise RuntimeError(f"没有这只宠物: {pet_id}(可用: "
                               f"{', '.join(e['id'] for e in self.entries()) or '无'})")
        if entry.get("error"):
            raise RuntimeError(f"{pet_id}: {entry['error']}")

        cached = self.cache_dir / f"{pet_id}.pet"
        if entry.get("cached") and cached.is_file():
            return cached.read_bytes(), self._meta(pet_id, entry)

        try:
            import gen_pet_package
        except ImportError as exc:
            raise RuntimeError(f"找不到 gen_pet_package: {exc}") from exc

        # Pillow 查在这里而不是靠 build_package 抛 ImportError: gen_pet_package 是
        # **惰性**导入 PIL 的(为了让 tests/test_pet_package.py 在没有 Pillow 的解释器上
        # 也能跑), 所以缺 Pillow 时抛出来的那条 ImportError 只会在切帧那一步冒出来,
        # 混在一堆别的原因里 —— 之前就变成过一句没头没脑的"内部错误"。
        if self._pil_version() is None:
            raise RuntimeError(
                f"打包 {pet_id} 需要 Pillow, 但桥接跑在 {sys.executable} 上, 它没有 Pillow。"
                f"在菜单栏的「选择 python3…」里换成装了 Pillow 的解释器, "
                f"或者给它装上: {sys.executable} -m pip install Pillow")

        try:
            packet, stats = gen_pet_package.build_package(Path(entry["source"]))
        except SystemExit as exc:  # 生成器用 SystemExit 报"这只不合适"
            raise RuntimeError(str(exc)) from exc

        if stats["total"] > PET_SLOT_BYTES:
            raise RuntimeError(
                f"包 {stats['total'] / 1024 / 1024:.2f} MiB 超过设备的宠物槽 "
                f"({PET_SLOT_BYTES / 1024 / 1024:.2f} MiB)")

        self.cache_dir.mkdir(parents=True, exist_ok=True)
        # 先写临时文件再改名: 中途崩了不会留下一个半截的缓存被当成"已生成"。
        tmp = cached.with_suffix(".pet.part")
        tmp.write_bytes(packet)
        tmp.replace(cached)
        (self.cache_dir / f"{pet_id}.stamp").write_text(entry["stamp"], encoding="utf-8")

        entry["cached"] = True
        entry["size"] = stats["total"]
        entry["stage"] = stats["stage"]
        return packet, self._meta(pet_id, entry)

    @staticmethod
    def _meta(pet_id: str, entry: dict) -> dict:
        return {"id": pet_id, "display": entry.get("display", pet_id),
                "total": entry.get("size", 0), "stage": entry.get("stage")}

    def describe(self) -> dict:
        """给控制通道/菜单用的列表。"""
        return {
            "pets_dir": str(self.pets_dir),
            "cache_dir": str(self.cache_dir),
            "slot_bytes": PET_SLOT_BYTES,
            "packer": self.packer(),
            "items": [
                {"id": item["id"], "display": item["display"],
                 "cached": bool(item.get("cached")), "size": item.get("size", 0),
                 "error": item.get("error")}
                for item in self.entries()
            ],
        }


class PetInstaller:
    """把"换宠物"做成一次后台传输, 顺便把进度广播出去。

    为什么必须在后台线程里跑: 一只宠物在 2.4 GHz Wi-Fi 上要传几十秒, 而设备每次
    收到一块都要先擦 flash 才写得进去。发在控制通道的读线程里会把那个客户端卡住
    那么久 —— 菜单栏看起来就像点了没反应。
    """

    def __init__(self, link: DeviceLink, library: PetLibrary) -> None:
        self.link = link
        self.library = library
        self._busy = threading.Lock()
        # 最近一次传输的结局, 供 snapshot 用。
        self.last: dict = {"id": "", "state": "idle", "sent": 0, "total": 0}

    @property
    def busy(self) -> bool:
        return self._busy.locked()

    def start(self, pet_id: str) -> str | None:
        """None 表示已经开始; 否则返回一句可以直接显示的失败原因。"""
        if not self.link.connected:
            return "设备还没连上"
        if not self._busy.acquire(blocking=False):
            return "已经有一次换宠物在进行"

        try:
            thread = threading.Thread(target=self._run, args=(pet_id,),
                                      daemon=True, name="pet-install")
            thread.start()
        except RuntimeError as exc:  # 起不了线程: 别把锁漏掉
            self._busy.release()
            return f"无法启动传输线程: {exc}"
        return None

    def _publish(self, event: dict) -> None:
        self.last = {**self.last, **{k: v for k, v in event.items() if k != "event"}}
        self.link._publish(event)

    def note_done(self, pet_id: str, ok: bool) -> None:
        """设备给出的结论。把 last 收尾成终局。

        不这么做的话 last 会停在 "sent", 而它正是客户端中途接入时用来对齐进度行的
        那一份 —— 用户会看到一行永远不动、也不告诉你结果的"等设备确认"。
        """
        total = self.last.get("total", 0)
        self.last = {"id": pet_id or self.last.get("id", ""),
                     "state": "done" if ok else "failed",
                     "sent": total if ok else self.last.get("sent", 0),
                     "total": total}
        if not ok:
            self.last["message"] = "设备拒收了这个包"

    def _run(self, pet_id: str) -> None:
        try:
            self._publish({"event": "petxfer", "id": pet_id, "state": "preparing",
                           "sent": 0, "total": 0})
            print(f"[pet] 准备 {pet_id}…", flush=True)
            packet, meta = self.library.package(pet_id)
            total = len(packet)
            self._publish({"event": "petxfer", "id": pet_id, "state": "sending",
                           "sent": 0, "total": total})
            print(f"[pet] 推送 {pet_id}: {total / 1024 / 1024:.2f} MiB", flush=True)

            ok = self.link.send_pet(pet_id, packet)
            if not ok:
                self._publish({"event": "petxfer", "id": pet_id, "state": "failed",
                               "sent": 0, "total": total,
                               "message": "链路中断, 设备那边槽是空的"})
                print("[pet] 传输中断", flush=True)
                return

            # 发完不等于装上: 设备还要核对长度与整包 CRC, 再重新映射。它会回一条
            # petdone —— 在那之前只能算"发出去了"。
            self._publish({"event": "petxfer", "id": pet_id, "state": "sent",
                           "sent": total, "total": total})
            print(f"[pet] {pet_id} 已发完, 等设备确认…", flush=True)
        except RuntimeError as exc:
            self._publish({"event": "petxfer", "id": pet_id, "state": "failed",
                           "sent": 0, "total": 0, "message": str(exc)})
            print(f"[pet] {exc}", flush=True)
        except Exception as exc:  # 传输线程不该把异常吞掉, 但也不能带崩进程
            self._publish({"event": "petxfer", "id": pet_id, "state": "failed",
                           "sent": 0, "total": 0, "message": f"内部错误: {exc}"})
            print(f"[pet] 内部错误: {exc}", flush=True)
        finally:
            self._busy.release()


def settle_pet_transfer(hub: ControlHub, installer: PetInstaller,
                        library: PetLibrary, pet_id: str, ok: bool) -> None:
    """设备给出结论(petdone)之后的收尾。

    两件事都是为了"半路连上来的客户端看到的仍然是对的":
    - `last` 收成终局, 不然它停在 "sent";
    - `petxfer` 报的是**进度不是状态**, 结论一到就从快照里撤掉 —— 菜单栏重放
      snapshot 时按事件名排序合并, `petdone` 排在 `petxfer` 前面, 留着那帧
      "sent" 会把它盖回去, 于是进度行永远停在"等设备确认"。
    顺手重扫列表: 打包成功后 cached 才翻真。

    单独成函数而不是写在 main 里的闭包, 是为了让它能被 tests/test_pet_bridge.py
    直接调 —— 这段"收尾"只有真机跑完一次传输才看得见, 靠人会漏。
    """
    installer.note_done(pet_id, ok)
    library.scan()
    hub.forget("petxfer")
    hub.publish({"event": "pets", **library.describe(), "last": installer.last})


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
    def _recent_rollouts(months: int = CODEX_RECENT_MONTHS,
                         days: int = CODEX_RECENT_DAYS) -> list[tuple[float, str]]:
        """最近几个日期目录里的 rollout 候选 `(mtime, 路径)`。

        Codex 按 `sessions/YYYY/MM/DD/rollout-*.jsonl` 存放会话, 跑久了会有上千个
        文件。这里只下探"最近的年 → 最近 N 个月 → 里面最新的 N 天", 目录项一律走
        `os.scandir`/`DirEntry.stat()`, 不重复 `stat` 已知项。

        根目录**从 `CODEX_SESSIONS_GLOB` 推导**, 不另设常量: 两处定义迟早对不上,
        而测试里换个目录就是改 glob —— 快速路径必须跟着走。

        刻意的取舍: 只认最近几天, 所以"最近几天压根没跑过 Codex"时这里会返回空,
        由调用方回退到递归 glob。省 CPU 不能以漏掉会话为代价。
        """
        root = CODEX_SESSIONS_GLOB.partition("/**/")[0]
        if root == CODEX_SESSIONS_GLOB:      # glob 里没有 `/**/`: 当普通目录处理
            root = os.path.dirname(CODEX_SESSIONS_GLOB)

        found: list[tuple[float, str]] = []

        def collect_files(folder: str) -> None:
            with os.scandir(folder) as entries:
                for entry in entries:
                    if entry.name.startswith("rollout-"):
                        found.append((entry.stat().st_mtime, entry.path))

        def newest_dirs(parent: str, count: int) -> list[os.DirEntry]:
            with os.scandir(parent) as entries:
                dirs = [entry for entry in entries if entry.is_dir()]
            return sorted(dirs, key=lambda entry: entry.name)[-count:]

        # 根目录自己也扫一遍: 万一布局变了(rollout 直接落在 sessions/ 下, 与旧的
        # 年/月/日 目录并存), 目录树那几层看不见的东西至少还能在这里被发现。
        collect_files(root)
        for year in newest_dirs(root, 1):
            for month in newest_dirs(year.path, months):
                for day in newest_dirs(month.path, days):
                    collect_files(day.path)
        return found

    @staticmethod
    def newest_session() -> str | None:
        """最近写入的 rollout 文件。

        先走 `_recent_rollouts()` 快速路径; 拿不到候选(目录结构不符、或最近几天没
        跑过 Codex)就回退到递归 glob —— 那条老路保证行为不变, 不会因为省 CPU 而
        漏掉会话。跟随线程每 0.25 秒问一次这个问题, 旧实现每次都递归扫全部会话并
        逐个 `stat`(本机 598 个文件实测 7.2 ms/轮 ≈ 7.9% 单核, 整条链路的最大开
        销), 而答案几乎永远是同一个。
        """
        try:
            recent = CodexWatcher._recent_rollouts()
        except OSError:
            recent = []
        if recent:
            return max(recent, key=lambda item: item[0])[1]

        files = glob.glob(CODEX_SESSIONS_GLOB, recursive=True)
        if not files:
            return None
        return max(files, key=os.path.getmtime)

    def _emit(self, state: str, text: str | None) -> None:
        self._state = state
        self._last_event_at = time.monotonic()
        self.manual_until_event = False
        self.link.send_state(state, text, source="codex")

    @staticmethod
    def _record_boundary(path: str) -> int:
        """文件末尾**完整记录**的边界: 从 EOF 往回退到最后一个换行。

        `_offset` 的不变量是"永远落在一条记录的起点"。一个刚开的会话通常正写到
        一半, 直接取 EOF 会把那条记录劈成两半 —— 下一轮读到的残片必然解析失败。
        """
        size = os.path.getsize(path)
        if size == 0:
            return 0
        window = 65536
        with open(path, "rb") as handle:
            handle.seek(max(0, size - window))
            tail = handle.read()
        end = tail.rfind(b"\n")
        if end < 0:
            # 64 KB 里连一个换行都没有(不正常的日志): 宁可跳过, 不重放整段历史。
            return size
        return size - len(tail) + end + 1

    def _switch(self, path: str) -> None:
        self._current = path
        # 新会话从当前末尾开始跟, 不重放历史 —— 否则一连上就会看到几个月前的
        # 旧事件在屏幕上飞快地刷一遍。起点要对齐到记录边界, 见 _record_boundary()。
        try:
            self._offset = self._record_boundary(path)
        except OSError:
            self._offset = 0
        print(f"[codex] 跟随会话: {Path(path).name}", flush=True)
        self._publish({"event": "session", "name": Path(path).name, "path": path})

    def _handle_record(self, record: dict) -> None:
        """映射一条 rollout 记录。

        方法名**必须**避开 `_handle`: Python 3.13 的 `threading.Thread.__init__`
        会给每个实例挂上 `self._handle = _ThreadHandle()`。实例属性优先于类方法, 所以
        一个叫 `_handle` 的方法在 `Thread` 子类里根本调不到 —— `self._handle(record)`
        抛 `TypeError: '_thread._ThreadHandle' object is not callable`, 跟随线程在
        第一条记录上就崩掉, 设备从此再也收不到 Codex 状态。真机日志见
        `~/Library/Application Support/CodexPetBridge/bridge.log` (2026-09-20)。
        """
        rtype = record.get("type")
        payload = record.get("payload")
        payload = payload if isinstance(payload, dict) else {}
        ptype = payload.get("type")

        if payload.get("error"):
            self._emit("failed", message_text(payload["error"]))
            return

        if rtype == "event_msg" and ptype == "task_started":
            self._emit("working", "Codex 开始处理任务")
            return

        if rtype == "event_msg" and ptype == "task_complete":
            self._emit("ready", message_text(payload.get("last_agent_message")))
            return

        if rtype == "event_msg" and ptype == "item_completed":
            item = payload.get("item")
            item = item if isinstance(item, dict) else {}
            itype = item.get("type")
            if itype == "CommandExecution":
                # 命令正文同样可能是内容块; 取不到再退回一句通用文案。
                text = message_text(item.get("content"))
                self._emit("working", text or "正在执行命令")
            elif itype == "Reasoning":
                self._emit("working", "正在思考…")
            elif itype == "FileChange":
                self._emit("working", "正在修改文件")
            elif itype == "AgentMessage":
                # 正文包在内容块里(`[{"type": "Text", "text": …}]`), 由 message_text 取出。
                self._emit("working", message_text(item.get("content")))
            elif itype == "UserMessage":
                # 新的用户消息: 下一轮通常马上开始。
                self._emit("working", "收到新指令")
            else:
                # 认不出的条目类型: 只有确实是字符串才拿它当文案。这里原本是
                # `str(itype or ...)` —— 与 message_text 同一条规矩, 结构体绝不
                # str 上屏。
                self._emit("working",
                           itype if isinstance(itype, str) and itype else "工作中")
            return

        # 等用户确认 / 授权: 具体事件名随版本变化, 用关键词兜底匹配。
        name = f"{rtype}/{ptype}".lower()
        if any(word in name for word in ("approval", "elicit", "confirm", "input_request")):
            self._emit("waiting", "等待你确认")
            return

    def read_new_records(self) -> int:
        """读一次被跟随文件的新尾巴, 返回消费掉的字节数。

        **只消费到最后一个换行为止。** rollout 是边写边刷的, 一次读很可能正好落在
        一条记录的中间。旧实现把那段残片也算进 `_offset`, 补全之后它再也不会被解析
        (残片单独解 JSON 必然失败) —— 丢掉的若是 `task_complete`, 宠物就一直卡在
        `working`, 直到 45 秒的 idle 超时才动一下。留在 `_offset` 之前, 下一轮读到
        的就是完整的一行。

        单独成方法是为了能在主机上直接测: 跟随线程真跑起来才看得见的账, 靠人会漏。
        """
        assert self._current is not None
        size = os.path.getsize(self._current)
        # 文件被截断/轮转时重新定位到开头。
        if size < self._offset:
            self._offset = 0
        if size == self._offset:
            return 0

        with open(self._current, "rb") as handle:
            handle.seek(self._offset)
            raw = handle.read()
        end = raw.rfind(b"\n")
        if end < 0:
            return 0  # 整段都还没写完, 一个字都不消费。

        consumed = end + 1
        self._offset += consumed
        # 按 b"\n" 切, 不用 splitlines() —— 后者连 \r、\v、\x1c 一起切, 而 offset
        # 的算术只认换行。切法必须和记账方式一致。
        for line in raw[:consumed].split(b"\n"):
            if not line.strip():
                continue
            try:
                record = json.loads(line.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                continue
            self._handle_record(record)
        return consumed

    def maybe_go_idle(self) -> bool:
        """日志静了一段就切回 idle, 但**手工状态要留到下一个 Codex 事件**。

        `apply_state()` 明确承诺过这条优先级, 而 idle 同样是日志推断出来的, 也得让路。
        少了这个判断, 菜单里"推送状态"点完会被下一轮 idle 打回去: 桥接启动 45 秒后
        全局的 idle 条件就一直满足, 于是手工状态在 0.25 秒内消失, 看起来就是"点了没
        反应" —— 真机日志里那两条挨着的 `[state] working / ...` 与 `[state] idle`
        就是这么来的。
        """
        if self.manual_until_event or self._state == "idle":
            return False
        if time.monotonic() - self._last_event_at <= self.idle_after:
            return False
        self._emit("idle", "")
        return True

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

                self.read_new_records()
                self.maybe_go_idle()
            except OSError as exc:
                print(f"[codex] 读日志出错: {exc}", flush=True)
            time.sleep(0.25)


# ---------------------------------------------------------------------------
# 服务端
# ---------------------------------------------------------------------------
def serve(link: DeviceLink, bind: str, port: int,
          on_petdone: Callable[[str, bool], None] | None = None) -> None:
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

        threading.Thread(target=read_device, args=(link, conn, on_petdone),
                         daemon=True, name="device-reader").start()
        threading.Thread(target=ping_loop, args=(link,),
                         daemon=True, name="keepalive").start()


def read_device(link: DeviceLink, conn: socket.socket,
                on_petdone: Callable[[str, bool], None] | None = None) -> None:
    """读设备发回来的消息(hello / battery / poke / pong / petdone)。"""
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
                elif mtype == "petdone":
                    # 设备那边的真正结论: 它核对过长度和整包 CRC, 并且重新映射过了。
                    # id 是**设备槽里那一只**, 不是我们发过去的那只 —— 发错文件时
                    # 这就是唯一能看出来地方。
                    pet_id = message.get("id") or ""
                    ok = bool(message.get("ok"))
                    print(f"[recv] 换宠物{'成功' if ok else '失败'}: {pet_id}", flush=True)
                    link._publish({"event": "petdone", "id": pet_id, "ok": ok})
                    # 换宠物的收尾不在这条线上做完: last 要收成终局, 列表要重扫
                    # (打包成功后 cached 才翻真)。由 main 里那个回调统一处理。
                    if on_petdone is not None:
                        on_petdone(pet_id, ok)
                    # 成功时设备紧接着还会补发一条 hello, 菜单栏以那条为准更新"当前
                    # 是哪只"; 这里不重复上报, 免得两个来源打架。
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

    def forget(self, *names: str) -> None:
        """把某几类事件从"最新一份"里拿掉。

        给**有结论之后就过时**的事件用: petxfer 报的是进度, 设备给出 petdone 之后
        再留着最后一帧(sent), 新接入的客户端重放 snapshot 时(菜单栏按事件名排序
        合并) 就会拿它当最终状态 —— 那行进度会永远停在"等设备确认"。
        """
        with self._lock:
            for name in names:
                self._latest.pop(name, None)

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


def build_command_router(link: DeviceLink, watcher: "CodexWatcher | None",
                         library: "PetLibrary | None" = None,
                         installer: "PetInstaller | None" = None):
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

        # --- 宠物 ---
        if name == "petlist":
            if library is None:
                return {"event": "error", "message": "宠物库未启用"}
            # 重新扫一遍: 用户可能刚在 Codex 里下了新宠物, 不该要求重启桥接。
            library.scan()
            payload = library.describe()
            if installer is not None:
                payload["busy"] = installer.busy
                payload["last"] = installer.last
            link._publish({"event": "pets", **payload})
            return None

        if name == "pet":
            if library is None or installer is None:
                return {"event": "error", "message": "宠物库未启用"}
            pet_id = command.get("id")
            if not isinstance(pet_id, str) or not pet_id:
                return {"event": "error", "message": "缺少 id"}
            problem = installer.start(pet_id)
            if problem is not None:
                return {"event": "error", "message": problem}
            return None

        return {"event": "error", "message": f"未知命令: {name}"}

    return handle


# ---------------------------------------------------------------------------
# 手工命令
# ---------------------------------------------------------------------------
HELP = """可用命令:
  <state> [文字]      推一个状态; state ∈ idle/working/waiting/ready/failed
  text <文字>         只更新气泡文字, 不改状态
  pets                列出 Codex 里可用的宠物(~/.codex/pets)
  pet <id>            把某一只打包推给设备, 换掉槽里那只
  raw <JSON>          直接发一行 JSON
  status              打印当前连接与状态
  quit                退出
示例:
  working 正在重构登录模块
  ready 改完了, 跑一下测试
  text 只是换一句话
  pet sophie-portrait
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


def repl(link: DeviceLink, watcher: CodexWatcher | None,
         library: "PetLibrary | None" = None,
         installer: "PetInstaller | None" = None) -> None:
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
        if line == "pets":
            if library is None:
                print("[err] 宠物库未启用", flush=True)
                continue
            for item in library.describe()["items"]:
                mark = "已打包" if item["cached"] else "未打包"
                note = f"  ({item['error']})" if item["error"] else ""
                print(f"  {item['id']:<24} {item['display']:<12} {mark}{note}",
                      flush=True)
            continue

        head, _, rest = line.partition(" ")
        rest = rest.strip()

        if head == "text":
            link.send_text(rest)
            continue
        if head == "pet":
            if library is None or installer is None:
                print("[err] 宠物库未启用", flush=True)
                continue
            if not rest:
                print("[err] 用法: pet <id>; 敲 pets 看有哪些", flush=True)
                continue
            problem = installer.start(rest)
            if problem is not None:
                print(f"[err] {problem}", flush=True)
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
    parser.add_argument("--pets-dir", default=str(PETS_DIR),
                        help=f"Codex 的宠物源目录, 默认 {PETS_DIR}")
    parser.add_argument("--pet-cache", default=DEFAULT_PET_CACHE,
                        help=f"生成的 .pet 缓存目录, 默认 {DEFAULT_PET_CACHE}")
    parser.add_argument("--no-pets", action="store_true",
                        help="关掉换宠物能力(设备没刷 pets 分区时用)")
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

    # 宠物库。--no-pets 时整套换宠物能力关掉(那台设备可能没刷 pets 分区)。
    library: PetLibrary | None = None
    installer: PetInstaller | None = None
    if not args.no_pets:
        library = PetLibrary(Path(os.path.expanduser(args.pets_dir)),
                             Path(os.path.expanduser(args.pet_cache)))
        installer = PetInstaller(link, library)
        # 启动就扫一次: 控制通道一连上来就能拿到最新的列表(publish 早于客户端接入
        # 也没关系, ControlHub 会把它留作 snapshot 的一份)。
        found = library.scan()
        print(f"[pets] {len(found)} 只可用: "
              f"{', '.join(item['id'] for item in found) or '无'}", flush=True)
        print(f"[pets] 源 {library.pets_dir}", flush=True)
        print(f"[pets] 缓存 {library.cache_dir}", flush=True)
        if hub is not None:
            hub.publish({"event": "pets", **library.describe(),
                         "last": installer.last})
    else:
        print("[pets] 已禁用宠物库(--no-pets)", flush=True)

    # 收尾(见 settle_pet_transfer)。三样缺一就关掉, 与 --no-pets 是同一套条件。
    on_petdone: Callable[[str, bool], None] | None = None
    if hub is not None and installer is not None and library is not None:
        on_petdone = functools.partial(settle_pet_transfer, hub, installer, library)

    if hub is not None:
        hub.publish({"event": "watch", "enabled": watcher is not None})
        hub.on_command = build_command_router(link, watcher, library, installer)
        try:
            hub.start()
        except OSError as exc:
            # 控制通道只是附加能力, 打不开不该拖垮主服务(设备还得照常连)。
            print(f"[ctrl] 控制通道打开失败({exc}), 仅以终端模式运行", flush=True)
            link.on_event = None
            hub = None

    threading.Thread(target=serve, args=(link, args.bind, args.port, on_petdone),
                     daemon=True, name="server").start()

    if sys.stdin.isatty():
        try:
            repl(link, watcher, library, installer)
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
