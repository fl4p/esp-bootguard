#!/bin/bash
# Diagnose what the guarded bootloader leaves in the retained record, with raw
# bytes read back over a download-mode connection (a USB download reset keeps
# RTC FAST memory). Flashes a variant, boots it three ways, restores a backup.
#   source $IDF_PATH/export.sh
#   tools/diag_retained.sh /dev/cu.usbmodem1101 [variant] [backup.bin]
set -u
PORT=${1:?port}
VARIANT=${2:-healthy}
BACKUP=${3:-test-out/backup.bin}
cd "$(dirname "$0")/.." || exit 2

et() { python -m esptool --chip esp32s3 -p "$PORT" -b 460800 "$@"; }

# rtc_retain_mem_t at 0x600fffe8: partition(8) reboot_counter(2) flags(1) reserve(1) custom(8) crc(4)
dump_record() {
    echo "-- record (download reset, RTC memory kept):"
    et --before default_reset --after no_reset read_mem 0x600fffe8 2>&1 | grep -i "0x600fffe8 ="
    for a in 0x600fffec 0x600ffff0 0x600ffff4 0x600ffff8 0x600ffffc; do
        et --before no_reset --after no_reset read_mem "$a" 2>&1 | grep -i "$a ="
    done
    echo "   (0x600ffff0 = reboot_counter u16 | flags | reserve; 0x600ffff4 = magic u16 | crashes | trips;"
    echo "    0x600ffff8 = last_rom | last_hint | last_action | loading; 0x600ffffc = crc)"
}

boot_capture() {   # reset from an open handle and keep the bootloader's lines
    python tools/capture_reset.py --port "$PORT" --seconds "$1" ${2:-} 2>&1 | grep -E "rst:|bootguard|crashloop|boot: ESP-IDF"
}

echo "==== 1 flash $VARIANT (--after no_reset: the chip stays in download mode)"
(cd "examples/crashloop/build-$VARIANT" && et --before default_reset --after no_reset write_flash @flash_args 2>&1 | tail -1)
dump_record

echo "==== 2 boot from an open handle"
boot_capture 8
dump_record

echo "==== 3 boot again (the previous boot was guarded)"
boot_capture 8
dump_record

echo "==== 4 corrupt the retained CRC, then boot"
et --before no_reset --after no_reset write_mem 0x600ffffc 0xdeadbeef 2>&1 | tail -1
boot_capture 8
dump_record

echo "==== 5 esptool --after watchdog_reset, then listen"
et --before no_reset --after watchdog_reset chip_id 2>&1 | tail -1
boot_capture 12 --no-reset
dump_record

if [ "$BACKUP" = none ]; then
    echo "==== 6 no restore (backup 'none')"
else
    echo "==== 6 restore $BACKUP"
    et --before no_reset --after hard_reset write_flash 0 "$BACKUP" 2>&1 | tail -2
fi
