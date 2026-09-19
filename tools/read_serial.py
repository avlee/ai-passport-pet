#!/usr/bin/env python3
"""抓一段串口日志, 只保留我们关心的行。

Wi-Fi 驱动在联网时会打大量日志, 全量 dump 会把关键几行淹掉, 所以按关键字过滤。
用法: read_serial.py [秒数] [关键字,逗号分隔]

注意: 这个脚本会拉 DTR/RTS 触发一次复位, 为的是从第一条日志看起。只想观察设备
当前状态(比如怀疑它卡住了)时别用它 —— 复位会把现场清掉, 那种场合要"不碰 DTR/RTS"
地附着上去。
"""
import sys
import time

import serial

PORT = "/dev/cu.usbmodem2101"
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
KEYS = sys.argv[2].split(",") if len(sys.argv) > 2 else [
    "pet_app", "pet_provision", "pet_bridge", "pet_settings", "pet_ui",
]

ser = serial.Serial(PORT, 115200, timeout=0.3)
# 触发一次复位, 好从第一条日志看起。
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
ser.reset_input_buffer()

end = time.time() + SECONDS
while time.time() < end:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode("utf-8", "replace").rstrip("\r\n")
    if not line:
        continue
    if any(k in line for k in KEYS):
        print(line, flush=True)

ser.close()
