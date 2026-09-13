#!/bin/bash
# On-board test of the recovery action with the BLE OTA recovery app (examples/recovery).
# Flashes the recovery set plus a crash-looping main app, waits for the guard to boot the
# recovery app, pushes a healthy image over BLE with fugu-mppt-firmware's etc/ota_ble.py,
# and checks the new image boots.
#   source $IDF_PATH/export.sh
#   tools/recovery_test.sh PORT CRASHING_MAIN_BIN HEALTHY_MAIN_BIN [FUGU_DIR]
set -u
PORT=${1:?port}; CRASH_BIN=${2:?crash-looping main app}; GOOD_BIN=${3:?healthy main app}
FUGU=${4:-$HOME/dev/pv/fugu-mppt-firmware}
cd "$(dirname "$0")/.." || exit 2
R=examples/recovery/build
OUT=test-out/recovery; mkdir -p "$OUT"

et() { python -m esptool --chip esp32s3 -p "$PORT" -b 460800 "$@" 2>&1 | tee -a "${OUT:-test-out/recovery}/esptool.log" | tail -2; return "${pipestatus[1]:-${PIPESTATUS[1]:-0}}"; }

wait_port_free() {   # esptool cannot open a port a reader still holds
    for _ in $(seq 1 40); do
        lsof "$PORT" > /dev/null 2>&1 || return 0
        sleep 0.25
    done
    echo "warning: $PORT still held after 10 s: $(lsof "$PORT" | tail -1)"
}

listen() {   # passive reader: default port open (no reset), reopens across re-enumeration
    python - "$PORT" "$1" <<'PY'
import sys, time, serial
port, secs = sys.argv[1], float(sys.argv[2])
t0 = time.time(); s = None; buf = b""
while time.time() - t0 < secs:
    if s is None:
        try: s = serial.Serial(port, 115200, timeout=0.2)
        except (serial.SerialException, OSError): time.sleep(0.05); continue
    try: chunk = s.read(4096)
    except (serial.SerialException, OSError):
        try: s.close()
        except Exception: pass
        s = None; continue
    if not chunk: continue
    buf += chunk
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        print(f"{time.time()-t0:7.2f} {raw.decode('utf-8','replace').rstrip()}", flush=True)
PY
}

boot_after_esptool() {   # with esptool's stub attached, an RTS hard reset can leave the chip in the
    et --before no_reset --after watchdog_reset chip_id > /dev/null 2>&1   # ROM downloader; the watchdog boots it
    wait_port_free
    listen "$1"
}

echo "==== 1 flash the recovery set and the crash-looping main app"
et --before default_reset --after no_reset write_flash \
    0x0 "$R/bootloader/bootloader.bin" 0x8000 "$R/partition_table/partition-table.bin" \
    0xf000 "$R/ota_data_initial.bin" 0x2a0000 "$R/recovery.bin" 0x20000 "$CRASH_BIN" 2>&1 | tail -2

echo "==== 2 boot: expect three crashes, then the recovery app (reset from an open console)"
python tools/capture_reset.py --port "$PORT" --seconds 35 > "$OUT/trip.txt" 2>&1
grep -E "bootguard|crashloop: abort|recovery|nus:|mark_healthy|advertis" "$OUT/trip.txt" | cut -c1-200

echo "==== 3 push the healthy image over BLE while listening on the console"
listen 150 > "$OUT/push_console.txt" 2>&1 & LP=$!
sleep 2
LPY=$(pgrep -P "$LP" -f "python" | head -1)   # the reader itself, not the subshell around it
( cd "$FUGU" && .venv/bin/python -m etc.ota_ble -f -y "$GOOD_BIN" fugu-recovery ) > "$OUT/push_host.txt" 2>&1
PRC=$?
echo "push rc=$PRC"; tail -15 "$OUT/push_host.txt" | cut -c1-200
sleep 20
kill $LP ${LPY:-} 2>/dev/null; wait $LP 2>/dev/null
wait_port_free
echo "==== 4 console during and after the push"
grep -E "OTAB (READY|OK|FAIL)|restarting|bootguard|crashloop: (esp_reset|marked|alive)|nus:|Loaded app from" "$OUT/push_console.txt" | cut -c1-200 | head -40

wait_port_free
echo "==== 5 invalid recovery image: erase the start of the test partition, crash-looping main app"
et --before default_reset --after no_reset write_flash 0x20000 "$CRASH_BIN" 2>&1 | tail -1
et --before no_reset --after no_reset erase_region 0x2a0000 0x1000 2>&1 | tail -1
boot_after_esptool 20 > "$OUT/invalid_test.txt" 2>&1
grep -E "bootguard|crashloop: abort|no valid image|entering ROM download|rst:" "$OUT/invalid_test.txt" | cut -c1-200
wait_port_free
et --before no_reset --after no_reset read_mem 0x6000812c 2>&1 | grep -i "0x6000812c ="

echo "==== 6 crash-looping app in the test partition: fallback at the second trip"
et --before no_reset --after no_reset write_flash 0x2a0000 "$CRASH_BIN" 2>&1 | tail -1
boot_after_esptool 45 > "$OUT/crashing_recovery.txt" 2>&1
grep -E "bootguard|crashloop: (abort|esp_reset)|recovery|never marked|entering ROM download|rst:" "$OUT/crashing_recovery.txt" | cut -c1-200
wait_port_free
et --before no_reset --after no_reset read_mem 0x6000812c 2>&1 | grep -i "0x6000812c ="

echo "==== 7 leave the board with the recovery app in test and the healthy main app in ota_0"
et --before no_reset --after no_reset write_flash 0x2a0000 "$R/recovery.bin" 0x20000 "$GOOD_BIN" 2>&1 | tail -1
et --before no_reset --after watchdog_reset chip_id 2>&1 | tail -1
