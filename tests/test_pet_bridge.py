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
import functools
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(REPO_ROOT / "tests"))

import gen_pet_package  # noqa: E402  (path set up above)
import pet_bridge  # noqa: E402


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

            # 电量走独立事件名: 挤在 device 里会把上面那条 hello 顶掉(ControlHub
            # 每类事件只留最新一份), 菜单栏就再也看不到固件和宠物包了。
            writer_end.sendall(b'{"type":"battery","soc":95}\n')
            event = json.loads(reader.readline())
            assert event == {"event": "battery", "soc": 95}, event

            # 设备读不到电量时发 soc=null; 越界的值也按"读不到"处理, 不能原样透给 GUI。
            for payload in (b'{"type":"battery","soc":null}\n',
                            b'{"type":"battery","soc":150}\n',
                            b'{"type":"battery"}\n'):
                writer_end.sendall(payload)
                event = json.loads(reader.readline())
                assert event == {"event": "battery", "soc": None}, event
            writer_end.close()
        finally:
            if client is not None:
                client.close()
            local.close()
            peer.close()
            link.detach()


def test_pet_swap_end_to_end() -> None:
    """换宠物: 控制通道上的 pets / petxfer / petdone, 以及真正发到线上去的字节。

    为什么必须在这里钉: 菜单栏应用(工具目录下的 Swift)自己没有测试挂具, 它只认这几
    个事件的名字和字段。字段一改, Swift 那边是**静默**失效 —— 编译照过, 菜单里那
    一项就是不动或者空着。

    刻意不用 Pillow: 把 .pet 预放进缓存并按打包器的口径写好 stamp, 于是这只"宠物"
    的来源就是这个测试自己拼的字节, 走的仍然是真实的宣告 + 原始字节路径。
    """
    blob = bytes(range(256)) * 12            # 3 KB, 够跨过好几个发送分片
    frames = [{"offset": i * 256, "x": i, "y": 0, "w": 16, "h": 16,
               "duration": 100 + i} for i in range(12)]
    states = [{"name": "idle", "first": 0, "count": 12}]
    package, stats = gen_pet_package.assemble_package(
        "test-pet", "测试宠物", blob, frames, states)

    with tempfile.TemporaryDirectory(prefix="pet-swap-") as workdir:
        pets_dir = Path(workdir) / "pets"
        pet_dir = pets_dir / "test-pet"
        pet_dir.mkdir(parents=True)
        manifest = {"id": "test-pet", "displayName": "测试宠物",
                    "spriteVersionNumber": 2, "spritesheetPath": "spritesheet.webp"}
        (pet_dir / "pet.json").write_text(json.dumps(manifest), encoding="utf-8")
        (pet_dir / "spritesheet.webp").write_bytes(b"fake atlas")

        cache_dir = Path(workdir) / "cache"
        cache_dir.mkdir()
        (cache_dir / "test-pet.pet").write_bytes(package)
        # 缓存命中判据就是这份 stamp; 用库自己的算法算, 免得格式两边各写一份。
        (cache_dir / "test-pet.stamp").write_text(
            pet_bridge.PetLibrary._stamp(pet_dir, manifest), encoding="utf-8")

        library = pet_bridge.PetLibrary(pets_dir, cache_dir)

        socket_path = os.path.join(workdir, "bridge.sock")
        assert len(socket_path.encode()) < 104, "AF_UNIX 路径过长"

        link = pet_bridge.DeviceLink()
        installer = pet_bridge.PetInstaller(link, library)
        hub = pet_bridge.ControlHub(socket_path)
        link.on_event = hub.publish

        local, peer = socket.socketpair()
        client = None
        device_bytes = bytearray()
        device_error: list[str] = []

        def fake_device() -> None:
            """设备侧: 读一行宣告, 再按 size 收原始字节, 最后回 petdone。"""
            try:
                peer.settimeout(5.0)
                buffer = b""
                while b"\n" not in buffer:
                    buffer += peer.recv(4096)
                line, buffer = buffer.split(b"\n", 1)
                announce = json.loads(line.decode("utf-8").strip())
                assert announce["type"] == "pet", announce
                assert announce["id"] == "test-pet", announce
                assert announce["size"] == len(package), announce
                assert announce["crc32"] == zlib.crc32(package) & 0xFFFFFFFF, announce

                device_bytes.extend(buffer)
                while len(device_bytes) < announce["size"]:
                    chunk = peer.recv(65536)
                    if not chunk:
                        break
                    device_bytes.extend(chunk)

                peer.sendall(b'{"type":"petdone","id":"test-pet","ok":1}\n')
            except Exception as exc:   # 断言失败不能只留在子线程里
                device_error.append(f"{type(exc).__name__}: {exc}")

        try:
            with contextlib.redirect_stdout(io.StringIO()):
                link.attach(local, "192.168.0.108:58136")
            threading.Thread(target=pet_bridge.read_device,
                             args=(link, local,
                                   functools.partial(pet_bridge.settle_pet_transfer,
                                                     hub, installer, library)),
                             daemon=True, name="fake-device-reader").start()
            threading.Thread(target=fake_device, daemon=True,
                             name="fake-device").start()

            assert library.scan()[0]["cached"] is True, "缓存没命中, 后面会去调 Pillow"
            hub.on_command = pet_bridge.build_command_router(link, None, library,
                                                            installer)
            hub.start()

            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.settimeout(8.0)
            client.connect(socket_path)
            reader = client.makefile("rb")
            json.loads(reader.readline())      # 吃掉 snapshot

            def send(payload: dict) -> None:
                client.sendall(json.dumps(payload).encode() + b"\n")

            # 1) 列表: 菜单栏照着 items 建子菜单, 字段名错了那一项就是空的。
            send({"cmd": "petlist"})
            event = json.loads(reader.readline())
            assert event["event"] == "pets", event
            assert event["pets_dir"] == str(pets_dir), event
            assert event["busy"] is False, event
            assert event["slot_bytes"] == pet_bridge.PET_SLOT_BYTES, event
            assert event["items"] == [{"id": "test-pet", "display": "测试宠物",
                                       "cached": True, "size": len(package),
                                       "error": None}], event

            # 2) 换宠物: 成功命令没有回执, 只有事件。收集到 petdone 为止。
            send({"cmd": "pet", "id": "test-pet"})
            events = []
            while True:
                event = json.loads(reader.readline())
                events.append(event)
                if event["event"] == "petdone":
                    break

            kinds = [e["event"] for e in events]
            assert kinds[-1] == "petdone", kinds
            transfers = [e for e in events if e["event"] == "petxfer"]
            states_seen = [e["state"] for e in transfers]
            assert states_seen[0] == "preparing", states_seen
            assert "sending" in states_seen, states_seen
            assert states_seen[-1] == "sent", states_seen
            for entry in transfers:
                assert entry["id"] == "test-pet", entry
                assert entry["total"] in (0, len(package)), entry
                assert 0 <= entry["sent"] <= len(package), entry
            # 最后一条 sending 必须报满 —— 进度停在 99% 就是分片算错了。
            assert transfers[-2]["sent"] == len(package), transfers[-2]

            done = events[-1]
            assert done == {"event": "petdone", "id": "test-pet", "ok": True}, done

            # 3) 线上必须一个字节不差: 中间混进一行 JSON 就整包错位。
            assert not device_error, device_error
            assert bytes(device_bytes) == package, (
                f"设备收到 {len(device_bytes)} 字节, 包里是 {len(package)}")

            # 4) 收尾。设备给了结论之后, 半路连上来的客户端重放 snapshot 也必须看到
            #    终局: last 收成 done, 而且**不能**留着最后一帧 petxfer(sent) ——
            #    菜单栏按事件名排序合并快照, petdone 排在 petxfer 前面, 留着的
            #    "sent" 会把它盖回去, 那行进度就永远停在"等设备确认"。
            #
            #    settle 跑在设备读线程里, 与客户端读到的 petdone 之间**没有**先后保证,
            #    所以这里等它落地再断言。上一版没等, 于是偶发(其实是必发)假失败。
            def settled() -> dict:
                deadline = time.time() + 3.0
                while True:
                    state = hub.snapshot()["state"]
                    if state.get("pets", {}).get("last", {}).get("state") in ("done", "failed"):
                        return state
                    assert time.time() < deadline, "等收尾超时"
                    time.sleep(0.02)

            state = settled()
            assert state["pets"]["last"]["state"] == "done", state["pets"]["last"]
            assert "petxfer" not in state, sorted(state)
            # 打包成功后 cached 要翻真 —— 否则"已经打过的包"在菜单里还是"未打包"。
            assert state["pets"]["items"][0]["cached"] is True, state["pets"]["items"]
        finally:
            if client is not None:
                client.close()
            local.close()
            peer.close()
            link.detach()


def test_missing_pillow_is_explained_not_an_internal_error() -> None:
    """缺 Pillow 时必须说清"是解释器挑错了", 而不是一句"内部错误"。

    gen_pet_package 是**惰性**导入 PIL 的(为了让 tests/test_pet_package.py 在没有
    Pillow 的解释器上也能跑), 所以那句 ImportError 不是从 `import gen_pet_package`
    抛出来的 —— 兜在它上面的友好提示永远不会触发, 最后落进通用的 except Exception,
    变成 "内部错误: No module named 'PIL'"。真机上就是这么撞的: 菜单栏点一下没结果,
    日志里只有一句无从下手的"内部错误"。这里把它钉住。
    """
    pets_dir = Path(tempfile.mkdtemp(prefix="pets-nopil-"))
    cache_dir = Path(tempfile.mkdtemp(prefix="pets-nopil-cache-"))
    entry = pets_dir / "test-pet"
    entry.mkdir()
    (entry / "pet.json").write_text(json.dumps({
        "id": "test-pet", "displayName": "测试宠物", "spriteVersionNumber": 2,
        "spritesheetPath": "spritesheet.webp",
    }), encoding="utf-8")
    (entry / "spritesheet.webp").write_bytes(b"\x00" * 32)

    library = pet_bridge.PetLibrary(pets_dir=pets_dir, cache_dir=cache_dir)
    assert library.scan()[0]["cached"] is False, "缓存不该命中, 否则不会走到打包"

    # 假装这个解释器没有 Pillow。不能真卸 Pillow —— 别的用例要靠它建夹具。
    library._pil_version = staticmethod(lambda: None)   # type: ignore[method-assign]

    # 菜单栏靠 packer 在**点之前**就提示, 所以这里也得对。
    assert library.packer()["available"] is False
    assert "Pillow" in (library.describe()["packer"]["hint"] or "")

    try:
        library.package("test-pet")
    except RuntimeError as exc:
        message = str(exc)
        assert "Pillow" in message, message
        assert sys.executable in message, f"报错里要说清是哪个解释器: {message}"
        assert "内部错误" not in message, message
    else:
        raise AssertionError("没有 Pillow 却打包成功了?")


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
        ("pet_swap_end_to_end", test_pet_swap_end_to_end),
        ("missing_pillow_is_explained",
         test_missing_pillow_is_explained_not_an_internal_error),
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
