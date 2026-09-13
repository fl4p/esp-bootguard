# Recovery app: firmware over BLE after a crash loop

A small ESP-IDF app for the `test` partition. With `CONFIG_BOOTGUARD_ACTION_RECOVERY=y` the guarded
bootloader boots it after `CONFIG_BOOTGUARD_MAX_CRASHES` consecutive crash resets. It advertises
a BLE service, takes a firmware image through [esp-ota-ble](https://github.com/fl4p/esp-ota-ble)
and writes it to `ota_0`. Then it selects `ota_0` as the boot partition and reboots.

ESP32-S3, ESP-IDF v5.5, NimBLE host. The console stays on USB-Serial-JTAG.

## Layout (`partitions.csv`, 4 MB)

| name | type | subtype | offset | size |
|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 0x6000 |
| otadata | data | ota | 0xf000 | 0x2000 |
| phy_init | data | phy | 0x11000 | 0x1000 |
| ota_0 | app | ota_0 | 0x20000 | 0x280000 (main app) |
| test | app | test | 0x2a0000 | 0x160000 (this app) |

Why this layout works (ESP-IDF v5.5 sources):

- **The update target is `ota_0`.** esp-ota-ble picks its slot with
  `esp_ota_get_next_update_partition(NULL)`. That function walks the OTA subtypes from `ota_0`
  upwards. It returns the slot after the running one, or else the first OTA slot it found
  (`components/app_update/esp_ota_ops.c:737-781`). `test` is not an OTA subtype, so the running
  partition never matches, and the result is `ota_0`, the only OTA slot. `esp_ota_begin` refuses
  only the running partition (`esp_ota_ops.c:159-161`), so it accepts `ota_0`.
- **Selecting `ota_0` works.** `esp_ota_set_boot_partition(ota_0)` validates the image and
  rewrites otadata (`esp_ota_ops.c:599-635`).
- **The stock boot path boots `ota_0`.** In the bootloader, only OTA subtypes count towards
  `app_count`; `test` is only recorded (`components/bootloader_support/src/bootloader_utility.c:176-193`).
  If otadata is blank and there is no factory partition, it takes "No factory image, trying OTA 0"
  (`bootloader_utility.c:408-425`). Once otadata is written, the active entry wins.
- **`test` is reached only on purpose.** The bootloader loads `test` only when asked for
  `TEST_APP_INDEX` (`bootloader_utility.c:585-591`), or as a last resort when no other app will boot
  (`:622-624`). Both are what a recovery app wants: the boot guard asks for it
  (`bootloader_components/main/bootguard_boot.c:194-196`), and so does an empty or corrupt `ota_0`.
- **No `factory`, no `ota_1`.** A factory partition would take `FACTORY_INDEX` on blank otadata.
  With a second OTA slot, the push would not reliably target the main app.

## GATT layer

It matches fugu-mppt-firmware's `src/tele/console_ble.cpp:65-68,365-389` and its host tool
`etc/ota_ble.py:52-55`:

| | UUID | properties |
|---|---|---|
| service (advertised) | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
| RX, command lines | `6E400002-…` | write, write-without-response |
| TX, status lines | `6E400003-…` | notify; a byte stream in MTU-3 chunks |
| FW, firmware bytes | `6E400004-…` | write-without-response |

Commands arrive as `ota-ble begin|resume|end|abort|info …`, the prefix fugu's tool sends, or bare.
`ping` answers `pong`. The link is open, with no pairing. The device name is
`CONFIG_RECOVERY_BLE_NAME` (default `fugu-recovery`). It goes in the scan response; the service
UUID goes in the advertisement. With no name, fugu's tool picks a `fugu-*` device that advertises
the service.

After 10 s of uptime the app calls `bootguard_mark_healthy()` (`CONFIG_RECOVERY_HEALTHY_AFTER_MS`).

## Build

```sh
. ~/dev/esp/idf5.5/export.sh
cd examples/recovery
idf.py set-target esp32s3 && idf.py build
```

esp-ota-ble is expected at `~/dev/pv/esp-ota-ble`. Otherwise pass `-DESP_OTA_BLE_DIR=<path>` or set
the environment variable. Its tamp and esp_delta_ota dependencies come from the component manager.

## Flash

`idf.py flash` writes the app to the first app slot (`ota_0`), which is the wrong slot for this
app. Flash by offset instead.

1. Bootloader, partition table, blank otadata and this app, from `examples/recovery`:

   ```sh
   python -m esptool --chip esp32s3 -p PORT --before default_reset --after hard_reset write_flash \
     0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
     0xf000 build/ota_data_initial.bin 0x2a0000 build/recovery.bin
   ```

2. The main app, into `ota_0`. It must be built with this partition table and the same boot guard
   options; `sdkconfig.main-app` adds them. For example, the crashloop example:

   ```sh
   cd ../crashloop
   IDF_COMPONENT_MANAGER=0 idf.py -B /tmp/bootguard-main-abort -D SDKCONFIG=/tmp/bootguard-main-abort.sdkconfig \
     -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.kind_abort;../recovery/sdkconfig.main-app" build
   python -m esptool --chip esp32s3 -p PORT --before default_reset --after hard_reset write_flash \
     0x20000 /tmp/bootguard-main-abort/crashloop.bin
   ```

## Push an image

Use fugu-mppt-firmware's host tool, which drives `esp-ota-ble/host/esp_ota_ble.py`:

```sh
cd ~/dev/pv/fugu-mppt-firmware
python -m etc.ota_ble -f -y /path/to/image.bin fugu-recovery
```

- `-f` skips the version probe; this app does not answer `uptime`.
- `-y` accepts an image without BLE, which the tool otherwise refuses non-interactively.
- After `end`, the tool reconnects and asks `info` of the new image to prove it runs. An image
  without this GATT service, such as a crashloop app, cannot answer. The tool then prints
  "device did NOT come back" even though the flash and the reboot worked. Check the console.
  Pushing `build/recovery.bin` itself passes that verification: it comes up in `ota_0` and reports
  `run=ota_0`.
