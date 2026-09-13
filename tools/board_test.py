#!/usr/bin/env python3
"""On-board test of the boot guard: ESP32-S3 on its USB-Serial-JTAG port.

Flashes the crash-loop variants built by tools/build_variants.sh and checks the
console for the guard's own log lines, then restores the board's previous flash.

    source $IDF_PATH/export.sh
    tools/build_variants.sh
    tools/board_test.py --port /dev/cu.usbmodem1101

The first step reads the board's first 1.1 MB (bootloader, partition table,
a 1 MB factory app) to test-out/backup.bin; the last step writes it back. If a
run dies half way, restore with:  tools/board_test.py --port ... --restore-only
"""
import argparse
import os
import re
import subprocess
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLE = os.path.join(ROOT, "examples", "crashloop")
OUT = os.path.join(ROOT, "test-out")
BACKUP_LEN = 0x110000
OPTION1_REG = 0x6000812C   # RTC_CNTL_OPTION1_REG, bit 0 = RTC_CNTL_FORCE_DOWNLOAD_BOOT

MWDT_ROM = {0x07, 0x08, 0x0B, 0x11}   # main watchdog resets: counted without a hint
STATUS_RE = re.compile(r"crashloop: esp_reset_reason (\d+); guard (\S+): crashes (\d+) trips (\d+) last_rom 0x([0-9a-f]+) last_hint (\d+)")
ALIVE_RE = re.compile(r"crashloop: alive (\d+); esp_reset_reason (\d+); status (\S+) crashes (\d+); reboot_counter (\d+); raw magic 0x([0-9a-f]+) crashes (\d+) trips (\d+) last_rom 0x([0-9a-f]+) last_hint (\d+) loading (\d+)")
GUARD_RE = re.compile(r"bootguard: reset 0x([0-9a-f]+) hint (\d+)([^:]*): (counted|not counted|count cleared), (\d+) of (\d+)")


def esptool(port, *args, cwd=None, check=True, timeout=240):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "-b", "460800", *args]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=cwd)
    out = p.stdout + p.stderr
    if check and p.returncode != 0:
        raise RuntimeError(f"esptool {' '.join(args)} failed (rc {p.returncode}):\n{out[-1200:]}")
    return p.returncode, out


class Console:
    """The USB-Serial-JTAG console across chip resets: every reset re-enumerates
    the USB device, so a read error means reopen, not failure."""

    def __init__(self, port, log):
        self.port, self.log, self.s, self.buf = port, log, None, b""

    def close(self):
        if self.s is not None:
            try:
                self.s.close()
            except Exception:
                pass
        self.s = None

    def collect(self, done, timeout):
        t0 = time.time()
        lines = []
        while time.time() - t0 < timeout:
            if self.s is None:
                try:
                    self.s = serial.Serial(self.port, 115200, timeout=0.2)
                except (serial.SerialException, OSError):
                    time.sleep(0.05)
                    continue
            try:
                chunk = self.s.read(4096)
            except (serial.SerialException, OSError):
                self.close()
                continue
            if not chunk:
                continue
            self.buf += chunk
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self.log.write(f"{time.time() - t0:7.2f} {line}\n")
                self.log.flush()
                lines.append(line)
                if done(lines):
                    return True, lines
        return False, lines


def guard_lines(lines):
    return [m.groups() for m in map(GUARD_RE.search, lines) if m]


class Run:
    def __init__(self, port):
        self.port = port
        os.makedirs(OUT, exist_ok=True)
        self.log = open(os.path.join(OUT, "console.log"), "a")
        self.con = Console(port, self.log)
        self.results = []

    def note(self, name, ok, detail):
        self.results.append((name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)
        self.log.write(f"==== {'PASS' if ok else 'FAIL'} {name}: {detail}\n")
        self.log.flush()

    def flash(self, variant, after="hard_reset"):
        self.con.close()
        self.log.write(f"==== flash {variant} (--after {after})\n")
        self.log.flush()
        esptool(self.port, "--before", "default_reset", "--after", after, "write_flash", "@flash_args",
                cwd=os.path.join(EXAMPLE, f"build-{variant}"))

    def watch(self, done, timeout):
        return self.con.collect(done, timeout)

    # ---- steps -------------------------------------------------------------
    def backup(self):
        path = os.path.join(OUT, "backup.bin")
        if os.path.exists(path) and os.path.getsize(path) == BACKUP_LEN:
            self.note("backup", True, f"kept existing {path}")
            return
        self.con.close()
        esptool(self.port, "--before", "default_reset", "--after", "no_reset", "read_flash", "0", hex(BACKUP_LEN), path)
        self.note("backup", os.path.getsize(path) == BACKUP_LEN, f"{os.path.getsize(path)} B to {path}")

    def trip_to_download(self, variant, hints, allow_mwdt=False):
        self.flash(variant)
        ok, lines = self.watch(lambda ls: any("waiting for download" in l for l in ls), 60)
        g = guard_lines(lines)
        counts = [int(x[4]) for x in g]
        counted = [(int(x[0], 16), int(x[1])) for x in g if x[3] == "counted"]
        seen_hints = [h for _, h in counted]
        tripped = any("consecutive crash resets" in l for l in lines) and any("entering ROM download mode" in l for l in lines)
        self.note(f"{variant}: counts up and trips", ok and tripped and counts[-3:] == [1, 2, 3],
                  f"counts {counts}, counted hints {seen_hints}, rom download banner {'seen' if ok else 'NOT seen'}")
        good = [h in hints or (allow_mwdt and h == 0 and rom in MWDT_ROM) for rom, h in counted]
        self.note(f"{variant}: crash classified", bool(good) and all(good),
                  f"counted (rom, hint) {[(hex(r), h) for r, h in counted]}; accepted hints {sorted(hints)}"
                  + (" or a hint-less main-watchdog reset" if allow_mwdt else ""))
        self.con.close()
        rc, out = esptool(self.port, "--before", "no_reset", "--after", "no_reset", "read_mem", hex(OPTION1_REG), check=False)
        m = re.search(r"0x6000812c = (0x[0-9a-f]+)", out, re.I)
        val = int(m.group(1), 16) if m else None
        self.note(f"{variant}: chip is in download mode", rc == 0 and val is not None and (val & 1) == 1,
                  f"esptool --before no_reset connected rc {rc}, OPTION1 = {hex(val) if val is not None else '?'}")

    def download_survives_port_reopen(self):
        """After a trip, open and close the console port like a terminal would, then check the chip is still waiting."""
        self.trip_to_download("abort", {4})
        self.con.close()
        for _ in range(5):
            try:
                s = serial.Serial(self.port, 115200, timeout=0.5)
                time.sleep(0.4)
                s.read(4096)
                s.close()
            except (serial.SerialException, OSError) as e:
                self.log.write(f"==== reopen error: {e}\n")
            time.sleep(0.6)
        ok, lines = self.watch(lambda ls: any("bootguard: reset" in l for l in ls), 8)
        self.con.close()
        rc, out = esptool(self.port, "--before", "no_reset", "--after", "no_reset", "read_mem", hex(OPTION1_REG), check=False)
        m = re.search(r"0x6000812c = (0x[0-9a-f]+)", out, re.I)
        val = int(m.group(1), 16) if m else None
        self.note("download mode survives five console reopens", rc == 0 and val is not None and (val & 1) == 1 and not ok,
                  f"boot seen after reopens: {ok}; esptool no_reset rc {rc}, OPTION1 = {hex(val) if val is not None else '?'}")

    def watchdog_reset_not_counted(self):
        """esptool --after watchdog_reset on a healthy app. The healthy variant repeats its
        state every 5 s, so the check does not depend on the lines printed right after the reset."""
        self.flash("healthy")
        ok = self.watch(lambda ls: any("marked healthy (ESP_OK)" in l for l in ls), 30)[0]
        self.note("healthy: marks itself healthy", ok, "marked healthy (ESP_OK)" if ok else "not seen")
        self.con.close()
        esptool(self.port, "--before", "default_reset", "--after", "watchdog_reset", "chip_id")
        ok, lines = self.watch(lambda ls: any(ALIVE_RE.search(l) for l in ls), 30)
        alive = [m.groups() for m in map(ALIVE_RE.search, lines) if m]
        g = guard_lines(lines)
        if not alive:
            self.note("esptool watchdog reset is not counted", False, f"no status line within 30 s; guard line {g[-1] if g else 'lost'}")
            return
        n, why, status, crashes, counter, magic, rcrashes, trips, last_rom, last_hint, loading = alive[-1]
        detail = (f"esp_reset_reason {why}, status {status}, crashes {crashes}, reboot_counter {counter}, "
                  f"raw magic 0x{magic} crashes {rcrashes} trips {trips} last_rom 0x{last_rom} last_hint {last_hint} loading {loading}; "
                  f"guard line {g[-1] if g else 'lost'}")
        self.note("esptool watchdog reset is not counted", int(rcrashes) == 0 and (status != "ESP_OK" or int(crashes) == 0), detail)
        self.note("record valid in the app after esptool's watchdog reset", status == "ESP_OK", detail)

    def restart_not_counted(self):
        self.flash("restart")
        ok, lines = self.watch(lambda ls: len([x for x in guard_lines(ls) if x[3] == "not counted"]) >= 6, 40)
        g = guard_lines(lines)
        tripped = any("consecutive crash resets" in l for l in lines)
        self.note("restart: deliberate restarts do not count", ok and not tripped and all(int(x[4]) == 0 for x in g),
                  f"{len(g)} boots, verdicts {[x[3] for x in g]}")

    def two_crashes_then_healthy(self):
        self.flash("twocrash")
        ok, lines = self.watch(lambda ls: any("after the deliberate restart the count is" in l for l in ls), 40)
        g = guard_lines(lines)
        marked = any("marked healthy (ESP_OK)" in l for l in lines)
        final = [l for l in lines if "after the deliberate restart the count is" in l]
        self.note("twocrash: healthy mark clears the count", ok and marked and final and final[-1].endswith(" 0"),
                  f"counts {[int(x[4]) for x in g]}, marked {marked}, final '{final[-1] if final else ''}'")

    def halt(self):
        self.flash("abort-halt")
        ok, lines = self.watch(lambda ls: len([l for l in ls if "bootguard: halted after" in l]) >= 2, 45)
        self.note("abort-halt: halts after the limit and stays", ok, f"{len([l for l in lines if 'halted after' in l])} halt lines")
        self.con.close()
        rc = esptool(self.port, "--before", "default_reset", "--after", "no_reset", "chip_id", check=False)[0]
        self.note("abort-halt: host resets it into download mode", rc == 0, f"esptool default_reset rc {rc}")

    def restore(self):
        path = os.path.join(OUT, "backup.bin")
        self.con.close()
        esptool(self.port, "--before", "default_reset", "--after", "hard_reset", "write_flash", "0", path)
        lines = self.watch(lambda ls: len(ls) >= 40, 25)[1]
        self.note("restore", True, f"wrote {path}; {len(lines)} console lines after reset (see test-out/console.log)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--restore-only", action="store_true")
    ap.add_argument("--no-restore", action="store_true")
    ap.add_argument("--only", default="", help="comma-separated steps: abort,reopen,restart,twocrash,watchdog,stack,intwdt,halt")
    a = ap.parse_args()
    r = Run(a.port)
    try:
        if a.restore_only:
            r.restore()
            return 0
        steps = {
            "abort": lambda: r.trip_to_download("abort", {4}),                    # ESP_RST_PANIC
            "reopen": r.download_survives_port_reopen,
            "restart": r.restart_not_counted,
            "twocrash": r.two_crashes_then_healthy,
            "watchdog": r.watchdog_reset_not_counted,
            "stack": lambda: r.trip_to_download("stack", {4}, allow_mwdt=True),  # panic, or measured: TG1 WDT with no hint
            "intwdt": lambda: r.trip_to_download("intwdt", {5}),                  # ESP_RST_INT_WDT
            "halt": r.halt,
        }
        chosen = [x for x in a.only.split(",") if x] or list(steps)
        unknown = [x for x in chosen if x not in steps]
        if unknown:
            raise SystemExit(f"unknown steps {unknown}")
        r.backup()
        for name in chosen:
            steps[name]()
    except Exception as e:
        r.note("run", False, f"aborted: {e}")
    finally:
        if not a.no_restore and os.path.exists(os.path.join(OUT, "backup.bin")):
            try:
                r.restore()
            except Exception as e:
                r.note("restore", False, str(e))
    failed = [n for n, ok, _ in r.results if not ok]
    print(f"\n{len(r.results) - len(failed)} passed, {len(failed)} failed" + (f": {failed}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
