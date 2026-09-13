# esp-bootguard

A second-stage bootloader for the **ESP32-S3** (ESP-IDF v5.5) that counts
consecutive crash resets and, after a limit (default 3), stops the boot loop:

- **download** (default): sets the ROM's force-download flag and resets, so the
  chip waits in ROM download mode for esptool,
- **halt**: parks in the bootloader with its watchdog off,
- **recovery**: boots the app partition of subtype `test`, e.g. a small app that
  accepts firmware over BLE. It falls back to download (or halt) when that
  partition is missing, holds no valid image, or its app booted at the previous
  trip and never marked itself healthy.

It replaces ESP-IDF's bootloader `main` component with ESP-IDF's own
`bootloader_start.c` plus one call between partition selection and image load.
The application side is a tiny component that marks a boot healthy.

## Why

An app that crashes at startup reboots every second or two. On the S3's
built-in USB-Serial-JTAG port every reset re-enumerates the USB device, and a
flash tool racing that loop can fail with `No serial data received` until
someone holds BOOT and replugs. After the limit the loop stops, the port stays
put, and `esptool write_flash` just works.

## What counts as a crash

The bootloader reads the ROM reset reason and the reset hint ESP-IDF's panic
handler writes before it resets (`RTC_CNTL_STORE6_REG`, see
`esp_system/port/soc/esp32s3/reset_reason.c`).

| reset | count |
|---|---|
| panic, interrupt watchdog, task watchdog, other watchdog, CPU lockup (hint) | +1 |
| main-watchdog reset without a hint (measured: a stack overflow resets through the timer group 1 watchdog and never reaches the panic handler) | +1 |
| software or RTC-watchdog reset without a hint while the app never started | +1 |
| no bootable app (partition table or OTA data unusable) | +1 |
| power-on, brownout, super watchdog (one ROM code), reset from the USB host | cleared |
| esptool's `--after watchdog_reset` (measured: the bootloader reads ROM reset code 0x01, the power-on code) | cleared |
| `esp_restart()`, deep-sleep wake, anything else | unchanged |
| `bootguard_mark_healthy()` from the app | cleared |

"The app never started" is a `loading` flag the bootloader sets before it hands
over and the app component clears from a startup constructor. That is how a
hint-less software reset from a failed image load counts while a deliberate
`esp_restart()` does not: ESP-IDF writes no hint for either.

The count lives in the `custom` area of ESP-IDF's retained RTC FAST memory
(`rtc_retain_mem_t`), which survives every reset except a loss of the RTC
domain. ESP-IDF 5.x leaves that area out of the struct's CRC unless
`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC` is set, so the guard requires the
option: with it, a corrupted or foreign record reads as invalid and the count
starts over rather than acting on garbage. When the limit is reached the count
restarts at zero, so a freshly flashed image gets a full set of tries.

## Use it

1. Point your project at both directories (paths relative to your project):

   ```cmake
   cmake_minimum_required(VERSION 3.16)
   set(EXTRA_COMPONENT_DIRS "path/to/esp-bootguard/components")
   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
   idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "path/to/esp-bootguard/bootloader_components" APPEND)
   project(my_app)
   ```

2. Reserve the retained memory in `sdkconfig.defaults`:

   ```
   CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
   CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x8
   CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC=y
   ```

   The build stops with an `#error` if any of them is missing.

3. Add `REQUIRES bootguard` to your main component and mark a boot good once
   the app has proven itself:

   ```c
   #include "bootguard.h"
   // e.g. after the main loop has run a few seconds
   bootguard_mark_healthy();
   ```

   `bootguard_get_status()` returns the count, the number of trips, the last
   reset reason and action, and whether the guard ran on this boot, for logging.

   **Every app booted by this bootloader must link the component**, including a
   recovery app. Its startup constructor clears the bootloader's hand-over flag;
   without it, three plain `esp_restart()` calls trip the guard.

4. Flash the bootloader once (`idf.py flash` writes it at 0x0).

### Leaving download mode

esptool's `--after hard_reset` clears the force-download flag before it resets
the chip, so a normal `idf.py flash` or `esptool ... write_flash ...` boots the
new image. `--after watchdog_reset` does not clear it (esptool source). A power
cycle should, since the flag lives in the RTC domain; that was not measured.

## Options

`menuconfig` → Component config → Boot guard:

| option | default |
|---|---|
| `CONFIG_BOOTGUARD_ENABLE` | y |
| `CONFIG_BOOTGUARD_MAX_CRASHES` | 3 |
| `CONFIG_BOOTGUARD_ACTION_DOWNLOAD` / `_HALT` / `_RECOVERY` | download |
| `CONFIG_BOOTGUARD_RECOVERY_FALLBACK_DOWNLOAD` / `_HALT` (no `test` partition) | download |

The options are defined in the app component so they reach the project's
`sdkconfig`: the bootloader subproject only keeps the symbols its own Kconfig
tree knows, and defining them there alone left every option at its default in
the bootloader build.

## BLE

Not in the bootloader. The second-stage bootloader runs before FreeRTOS, with no
heap, no radio calibration and no BLE controller, from a flash region that ends
at the partition table (0x8000 by default, about 32 KB). A Bluetooth stack is
hundreds of kilobytes and needs the full runtime.

The recovery action is the way to get there: a small BLE OTA app in a
partition of subtype `test`, which the bootloader boots after the limit instead
of the crashing app. `examples/recovery` is that app: a NimBLE GATT service
driving the [esp-ota-ble](https://github.com/fl4p/esp-ota-ble) receiver, with a
partition table (`ota_0` for the main app, `test` for the recovery app) and the
settings a main app needs to use it. It has no pairing or encryption: anyone in
range can push firmware while it runs.

Measured on the second board (2026-09-13, `tools/recovery_test.sh`):

| step | console |
|---|---|
| main app in `ota_0` aborts three times | `reset 0x0c hint 4: counted, 1 of 3` ... `3 of 3`, then `booting the test partition (recovery)` |
| recovery app starts | `guard on: crashes 0 trips 1 ... last_action recovery`, `update target ota_0`, `advertising as 'fugu-recovery'` |
| push a healthy main app from a Mac with fugu-mppt-firmware's `etc/ota_ble.py` | `OTAB READY part=ota_0 size=122101 xform=tamp out=197344`, about 3.5 s later `OTAB OK rebooting` |
| reboot into the pushed image | `reset 0x0c hint 0: not counted`, `Loaded app from partition at offset 0x20000`, then `marked healthy (ESP_OK)` and `alive` lines for 85 s |

The host tool then reported failure only because the pushed test image has no
BLE code, so it never advertised again for the tool's own reconnect check.

## Limitations

- ESP32-S3 only. The hint register and the force-download register are target
  specific; other targets stop the build with an `#error`.
- `bootloader_start.c` is a copy of ESP-IDF v5.5's. Re-diff it when you move to
  another ESP-IDF release.
- A hang the RTC watchdog resets **after** the app started is not counted: the
  rule only counts a hint-less RTC-watchdog reset while the app never started.
  ESP-IDF's interrupt and task watchdogs panic with a hint and are counted.
- A brownout loop is not counted (the ROM reports it as a power-on).
- A panic handler that stalls before it writes the reset hint (it prints first,
  with its RTC watchdog armed) ends in a hint-less RTC-watchdog reset, which is
  not counted.
- Download mode must be usable: eFuse-disabled download mode defeats the
  download action, and secure download mode refuses the register write that
  esptool's `hard_reset` uses to clear the force-download flag.
- The recovery action refuses to build with
  `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`: that fast path runs before the
  guard and would wake a sleeping recovery app into the cached crashing app.
- Download mode does not help when the USB-Serial-JTAG endpoint itself is dead
  (only a replug revives it). A BLE recovery app would.

## Test

`examples/crashloop` builds one app per behaviour (abort, stack overflow,
interrupt-watchdog hang, `esp_restart()`, two crashes then healthy, and abort
with the halt action). `tools/board_test.py` flashes them to a board on its
USB-Serial-JTAG port, checks the guard's console lines and the ROM download
state, and restores the board's previous flash from a backup at the end.

```sh
source $IDF_PATH/export.sh
tools/build_variants.sh
tools/board_test.py --port /dev/cu.usbmodem1101
```

Measured on two boards, ESP-IDF v5.5.1, console on USB-Serial-JTAG, macOS host,
2026-09-13: a Seeed XIAO ESP32-S3 (rev v0.2, 8 MB PSRAM), and a second
ESP32-S3 (QFN56 rev v0.1, 4 MB embedded flash, no usable BOOT button) that
ran the full test again after the two fixes below: **21 of 21 checks passed**.

| variant | what the bootloader logged | result |
|---|---|---|
| `abort()` | reset 0x0c hint 4 (panic): counted 1, 2, 3, then "entering ROM download mode" | ROM banner `DOWNLOAD(USB/UART0)`; esptool connected without a reset, force-download flag read back as 1 |
| stack overflow | reset 0x08 (timer group 1 watchdog) with **no** hint: counted 1, 2, 3 | tripped into download mode; the overflow never reached the panic handler, the main-watchdog rule caught it |
| interrupts off | reset 0x0c hint 5 (interrupt watchdog): counted 1, 2, 3 | tripped into download mode |
| `esp_restart()` loop | six boots, every one "not counted", count stayed 0 | never tripped |
| two aborts, then `bootguard_mark_healthy()` and one `esp_restart()` | counted 1, 2, then 0 after the deliberate restart | the healthy mark cleared the count |
| `abort()` with the halt action | "halted after 3 consecutive crash resets", repeated | stayed halted; esptool's default reset still put it into download mode |
| esptool `--after watchdog_reset` on a healthy app | ROM reset code 0x01 (power-on) | not counted; the app read a valid record with 0 crashes |
| console port opened and closed 5 times after a trip | no boot, no bootloader line | still in download mode: esptool connected without a reset, force-download flag still 1 |
| after a trip, nothing touches the chip for 40 s | silence | still in download mode, flag still 1 (before the watchdog fix below: it booted again 9.7 s after every trip) |
| recovery action with an erased test partition | `the test partition holds no valid image` | fell back to download mode |
| recovery action with a crash-looping app in the test partition | first trip boots it, the record reads `(recovery pending)`; at the next trip `the recovery app booted at the previous trip and never marked itself healthy` | fell back to download mode |
| blank retained memory (erased board), then a deliberately corrupted CRC | "count cleared" on each | the record read back over esptool after the app started: magic 0xb6a1, last reset 0x15 |

Console lines printed right after a reset are sometimes missing from the host
log, because each reset re-enumerates the USB device; the test judges each step
by the lines that did arrive and by esptool's read of the chip.
`tools/diag_retained.sh` boots a variant three ways and reads the retained
record's raw bytes back over a download-mode connection;
`tools/capture_reset.py` resets the chip from an already open console so the
bootloader's own lines are kept.

### Found while testing

- **Download mode did not survive unattended.** The bootloader arms the RTC
  watchdog for its own run (`CONFIG_BOOTLOADER_WDT_TIME_MS`, 9 s). Nothing feeds
  it in ROM download mode, and its reset clears the RTC domain along with the
  force-download flag, so the chip booted back into its crash loop 9.7 s after
  every trip. Every earlier test missed it because esptool attached within a
  second or two of the trip. The guard now disables that watchdog before it sets
  the flag, as the halt action already did. This also explains a one-off exit
  from download mode seen earlier on the XIAO.

- **The record had no integrity check.** ESP-IDF 5.x leaves
  `rtc_retain_mem_t.custom` out of the struct's CRC unless
  `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC` is set. The guard now requires it.
- **The first boot after the retained memory was lost wiped the record.** A
  zeroed `rtc_retain_mem_t` has a CRC of exactly `UINT32_MAX` (the ROM CRC-32 of
  zero bytes with an all-ones seed), which ESP-IDF's validity check rejects, so
  ESP-IDF's own update at image load reset the struct again and erased what the
  guard had just written. Measured on the second board: after an erase, the record
  read back as zero with a reboot counter of 1. The guard now lets ESP-IDF
  establish a valid struct right after its reset; ESP-IDF's reboot counter then
  reads 2 on that first boot instead of 1.

## Review

An independent Codex review (xhigh) of the first commit found nine issues; all
were checked against the code and resolved: [docs/review-fb6a673.md](docs/review-fb6a673.md).

## License

Apache-2.0, like ESP-IDF, from which `bootloader_components/main/bootloader_start.c` is taken.
