#!/usr/bin/env python3
"""Tests for the Mac-side Pet Bridge (tools/pet_bridge.py).

两件事:

1. 发送格式的单元检查: 文本截断不切开多字节字符、多余空白被压成单行、每条消息
   一行并以 CRLF 结束。
2. **两端互校**: 把 Pet Bridge 真正写到 socket 上的字节, 灌进设备侧的协议解析器
   (main/pet_protocol.c 编译出来的 tests/parse_pet_line.c) 再比对结果。这样
   "Mac 发的" 和 "设备收的" 由同一份真实实现互相校验, 不靠人工对协议文档。

This runs on the host and needs no ESP-IDF.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(REPO_ROOT / "tests"))

import pet_bridge  # noqa: E402  (path set up above)


def build_device_parser(workdir: str) -> str:
    """编译设备侧的协议解析器, 返回可执行文件路径。"""
    cc = os.environ.get("CC", "cc")
    exe = os.path.join(workdir, "parse_pet_line")
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{REPO_ROOT / 'main'}",
         str(REPO_ROOT / "tests" / "parse_pet_line.c"),
         str(REPO_ROOT / "main" / "pet_protocol.c"),
         str(REPO_ROOT / "main" / "pet_state.c"),
         "-o", exe],
        check=True,
    )
    return exe


def device_parse(exe: str, raw: bytes) -> list[dict]:
    """把字节交给设备解析器, 返回它读出来的消息列表。"""
    proc = subprocess.run([exe], input=raw, capture_output=True, check=True)
    messages = []
    for line in proc.stdout.decode("utf-8").splitlines():
        if not line:
            continue
        mtype, has_state, state, has_text, text = line.split(" ", 4)
        messages.append({
            "type": mtype,
            "state": state if has_state == "1" else None,
            "text": text if has_text == "1" else None,
        })
    return messages


def capture(peer: socket.socket) -> bytes:
    """取回对端刚收到的全部字节。

    DeviceLink 持有 socketpair 的一端, 写进去的字节要在另一端读 —— 这正好模拟
    "Mac 发、设备收"。
    """
    peer.setblocking(False)
    chunks = []
    try:
        while True:
            chunk = peer.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
    except BlockingIOError:
        pass
    finally:
        peer.setblocking(True)
    return b"".join(chunks)


def test_truncate_never_splits_a_character() -> None:
    # 每个汉字 3 字节; 200 个汉字远超 180 字节预算。
    text = "宠" * 200
    out = pet_bridge.truncate_utf8(text)
    assert len(out.encode("utf-8")) <= pet_bridge.TEXT_MAX_BYTES, len(out.encode())
    # 能解回 UTF-8 就说明没被切在字符中间。
    out.encode("utf-8").decode("utf-8")
    assert out.endswith("…")
    assert out[:-1] == "宠" * len(out[:-1])


def test_truncate_keeps_short_text() -> None:
    assert pet_bridge.truncate_utf8("正在重构登录模块") == "正在重构登录模块"


def test_squash_collapses_whitespace() -> None:
    assert pet_bridge.squash("a\nb\t c  d") == "a b c d"


def test_wire_format_and_device_roundtrip(exe: str) -> None:
    """Bridge 发出的字节必须能被设备解析器读成同样的状态和文本。"""
    cases = [
        ("idle", None),
        ("working", "正在重构登录模块"),
        ("waiting", "等待你确认"),
        ("ready", "改完了, 跑一下测试"),
        ("failed", "任务中断"),
        # 换行/制表符要被压平, 否则会撑破"一行一条消息"的前提。
        ("working", "first line\nsecond\tline"),
        # 超长中文要被截到 180 字节以内, 且不能切开字符。
        ("working", "很长的文本" * 40),
        # 含引号与反斜杠, 必须是合法 JSON。
        ("ready", 'he said "done" \\ ok'),
    ]

    for state, text in cases:
        local, peer = socket.socketpair()
        try:
            link = pet_bridge.DeviceLink()
            # DeviceLink 会往 stdout 打连接/状态日志, 测试时压掉保持输出干净。
            with contextlib.redirect_stdout(io.StringIO()):
                link.attach(local, "test")
                link.send_state(state, text)
            raw = capture(peer)
        finally:
            local.close()
            peer.close()

        assert raw.endswith(b"\r\n"), raw
        assert raw.count(b"\n") == 1, f"应只有一行: {raw!r}"

        payload = json.loads(raw.decode("utf-8").strip())
        assert payload["type"] == "state"
        assert payload["state"] == state

        parsed = device_parse(exe, raw)
        assert len(parsed) == 1, parsed
        assert parsed[0]["type"] == "state", parsed
        assert parsed[0]["state"] == state, (state, parsed)

        if text is None or not text.strip():
            assert parsed[0]["text"] is None, parsed
        else:
            expected = pet_bridge.truncate_utf8(pet_bridge.squash(text))
            # 设备侧必须原样拿到同一串文本(协议只做转义, 不改编码)。
            assert parsed[0]["text"] == expected, (expected, parsed)


def test_ping_and_poke_roundtrip(exe: str) -> None:
    local, peer = socket.socketpair()
    try:
        link = pet_bridge.DeviceLink()
        with contextlib.redirect_stdout(io.StringIO()):
            link.attach(local, "test")
            assert link.send({"type": "ping"})
            assert link.send_text("只是换一句话")
        raw = capture(peer)
    finally:
        local.close()
        peer.close()

    parsed = device_parse(exe, raw)
    assert [m["type"] for m in parsed] == ["ping", "text"], parsed
    assert parsed[1]["text"] == "只是换一句话", parsed


def test_send_without_device_is_safe() -> None:
    """设备没连上时发送必须是空操作, 不能抛异常。"""
    link = pet_bridge.DeviceLink()
    assert link.connected is False
    assert link.send_state("working", "x") is False
    assert link.send_text("x") is False


def test_server_accepts_device_and_delivers_state() -> None:
    """跑一遍真实的 serve(): 等设备连进来 -> 收 hello -> 推状态出去。"""
    link = pet_bridge.DeviceLink()

    # 先占一个空闲端口再放掉, 避免和别的进程抢。
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    threading.Thread(target=pet_bridge.serve, args=(link, "127.0.0.1", port),
                     daemon=True, name="test-server").start()

    client = None
    for _ in range(100):
        try:
            client = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            break
        except OSError:
            time.sleep(0.05)
    assert client is not None, "服务端没有在 5 秒内开始监听"

    try:
        # 设备侧握手: 固件连上后会先发 hello。
        client.sendall(json.dumps(
            {"type": "hello", "fw": "test", "pet": "test"}).encode() + b"\n")

        for _ in range(100):
            if link.connected:
                break
            time.sleep(0.05)
        assert link.connected, "accept 之后 DeviceLink 应该处于已连接"

        with contextlib.redirect_stdout(io.StringIO()):
            assert link.send_state("working", "正在重构登录模块")

        client.settimeout(2.0)
        payload = json.loads(client.recv(4096).decode("utf-8").strip())
        assert payload == {"type": "state", "state": "working",
                           "text": "正在重构登录模块"}, payload
    finally:
        client.close()
        link.detach()


def test_control_channel_drives_the_device() -> None:
    """控制通道: 连上先拿到全量状态, 下发的命令真的走到了设备。

    这是菜单栏应用唯一依赖的接口, 所以按"能被 GUI 用"的标准测: 事件流、命令、
    以及"正常命令没有回执、只有出错才回 error"这条契约。
    """
    with tempfile.TemporaryDirectory(prefix="ctl-") as workdir:
        socket_path = os.path.join(workdir, "bridge.sock")
        assert len(socket_path.encode()) < 104, "AF_UNIX 路径过长, 换个短一点的目录"

        link = pet_bridge.DeviceLink()
        hub = pet_bridge.ControlHub(socket_path)
        link.on_event = hub.publish

        local, peer = socket.socketpair()
        client = None
        try:
            # 先接上设备(attach 自己会广播一条 link 事件), 再补 bridge 事件 ——
            # publish 可以早于 hub.start(): 新客户端接入时靠这些拼出 snapshot。
            with contextlib.redirect_stdout(io.StringIO()):
                link.attach(local, "192.168.0.108:58136")
            hub.publish({"event": "bridge", "running": True, "pid": 1, "port": 8765})

            watcher = pet_bridge.CodexWatcher(link, 45.0, on_event=hub.publish)
            hub.on_command = pet_bridge.build_command_router(link, watcher)
            hub.start()

            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.settimeout(3.0)
            client.connect(socket_path)
            reader = client.makefile("rb")

            snapshot = json.loads(reader.readline())
            assert snapshot["event"] == "snapshot", snapshot
            assert snapshot["protocol"] == pet_bridge.CONTROL_PROTOCOL, snapshot
            assert snapshot["state"]["bridge"]["running"] is True, snapshot
            assert snapshot["state"]["link"]["peer"] == "192.168.0.108:58136", snapshot

            def send(payload: dict) -> None:
                client.sendall(json.dumps(payload).encode() + b"\n")

            send({"cmd": "state", "state": "working", "text": "来自菜单栏"})
            event = json.loads(reader.readline())
            assert event == {"event": "state", "state": "working", "text": "来自菜单栏",
                             "source": "manual", "delivered": True}, event
            # 命令不只是"改了个本地状态", 报文确实发到设备那头了。
            wire = json.loads(capture(peer).decode("utf-8").strip())
            assert wire == {"type": "state", "state": "working", "text": "来自菜单栏"}, wire

            # 只改文字用的是另一种事件, 不能把最近一次 state 从快照里顶掉。
            send({"cmd": "text", "text": "只改文字"})
            event = json.loads(reader.readline())
            assert event["event"] == "text", event
            send({"cmd": "snapshot"})
            event = json.loads(reader.readline())
            assert event["state"]["state"]["state"] == "working", event

            send({"cmd": "watch", "enabled": False})
            event = json.loads(reader.readline())
            assert event == {"event": "watch", "enabled": False}, event
            assert watcher.enabled is False

            # 未完成的命令只有 error; 成功时不发回执。
            send({"cmd": "nope"})
            event = json.loads(reader.readline())
            assert event["event"] == "error", event
            send({"cmd": "state", "state": "没这个状态"})
            event = json.loads(reader.readline())
            assert event["event"] == "error", event

            send({"cmd": "ping"})
            assert json.loads(reader.readline())["event"] == "pong"

            # 设备上报 hello 时, 控制通道必须能拿到固件与宠物包 —— 菜单栏要显示它们。
            device_end, writer_end = socket.socketpair()
            threading.Thread(target=pet_bridge.read_device,
                             args=(link, device_end), daemon=True).start()
            writer_end.sendall(b'{"type":"hello","fw":"0.1.0",'
                               b'"pet":"sophie-portrait"}\n')
            event = json.loads(reader.readline())
            assert event == {"event": "device", "kind": "hello", "fw": "0.1.0",
                             "pet": "sophie-portrait"}, event
            writer_end.close()
        finally:
            if client is not None:
                client.close()
            local.close()
            peer.close()
            link.detach()


def main() -> int:
    failures = []
    tests = [
        ("truncate_never_splits_a_character", test_truncate_never_splits_a_character),
        ("truncate_keeps_short_text", test_truncate_keeps_short_text),
        ("squash_collapses_whitespace", test_squash_collapses_whitespace),
        ("send_without_device_is_safe", test_send_without_device_is_safe),
        ("server_accepts_device_and_delivers_state",
         test_server_accepts_device_and_delivers_state),
        ("control_channel_drives_the_device", test_control_channel_drives_the_device),
    ]

    with tempfile.TemporaryDirectory(prefix="pet-bridge-") as workdir:
        exe = build_device_parser(workdir)
        tests.append(("wire_format_device_roundtrip",
                      lambda: test_wire_format_and_device_roundtrip(exe)))
        tests.append(("ping_and_text_device_roundtrip",
                      lambda: test_ping_and_poke_roundtrip(exe)))

        for name, test in tests:
            try:
                test()
                print(f"  ok   {name}")
            except AssertionError as exc:
                failures.append((name, exc))
                print(f"  FAIL {name}: {exc}")

    if failures:
        print(f"FAIL: {len(failures)} 个用例未通过", file=sys.stderr)
        return 1
    print("Pet bridge tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
