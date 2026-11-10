"""Capture a bounded startup-log window from the device console (read-only).

Usage: capture_serial.py [PORT] [SECONDS] [--reset]

--reset pulses RTS, which resets the chip through the USB-Serial-JTAG bridge so
the capture includes the boot log instead of only idle output.
"""
import re
import sys
import time

import serial

args = [a for a in sys.argv[1:] if a != "--reset"]
do_reset = "--reset" in sys.argv[1:]
PORT = args[0] if len(args) > 0 else "COM3"
SECONDS = float(args[1]) if len(args) > 1 else 15.0

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
# RTS 在本板 USB-Serial-JTAG 上接的是芯片 EN。实测必须在 open() **之前**就把
# RTS 置高（保持复位态）再释放，复位才真正生效；打开后再拉一下 RTS 经常抓不到
# 启动日志（表现为读到 0 字节，很容易被误判成固件刷坏）。
# 空闲时固件不打周期性日志，所以"复位 + 抓 boot log"是确认设备在跑的唯一办法。
ser.dtr = False
ser.rts = bool(do_reset)
ser.open()

if do_reset:
    time.sleep(0.15)
    ser.rts = False
    time.sleep(0.05)

buf = bytearray()
deadline = time.time() + SECONDS
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf.extend(chunk)
        # 边收边吐：长窗口（等用户去手机上点配对）时不必等到结束才有内容可看。
        text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", chunk.decode("utf-8", errors="replace"))
        sys.stdout.write(text)
        sys.stdout.flush()
ser.close()

sys.stdout.write("\n--- bytes: %d ---\n" % len(buf))
sys.stdout.flush()
