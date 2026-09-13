#!/usr/bin/env python3
"""Reset an ESP32-S3 on its USB-Serial-JTAG port from an already open console
handle and print what it logs, including the ROM banner and the second-stage
bootloader lines that are lost when the port is reopened after an esptool reset.

    tools/capture_reset.py --port /dev/cu.usbmodem1101 [--seconds 6] [--no-reset]

DTR stays deasserted (GPIO0 high, a normal boot) while RTS pulses EN. The USB
device re-enumerates on the reset, so the reader reopens the port and keeps
reading until the time is up.
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--no-reset", action="store_true", help="only listen")
    a = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.1
    s.dtr, s.rts = False, False
    deadline = time.time() + 5.0
    while True:   # the port can be mid re-enumeration (e.g. right after an esptool reset)
        try:
            s.open()
            break
        except (serial.SerialException, OSError):
            if time.time() > deadline:
                raise
            time.sleep(0.05)
    if not a.no_reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    t0 = time.time()
    buf = b""
    while time.time() - t0 < a.seconds:
        try:
            chunk = s.read(4096)
        except (serial.SerialException, OSError):
            try:
                s.close()
            except Exception:
                pass
            time.sleep(0.02)
            try:
                s = serial.Serial(a.port, 115200, timeout=0.1)
            except (serial.SerialException, OSError):
                pass
            continue
        if chunk:
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                print(f"{time.time() - t0:6.2f} {raw.decode('utf-8', 'replace').rstrip()}", flush=True)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
