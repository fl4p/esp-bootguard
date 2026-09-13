# Codex review of fb6a673 (2026-09-13)

Reviewer: codex-cli 0.154.0, model gpt-6-astra at reasoning effort xhigh, with network access and a
headed browser attached over CDP. Access checked independently: the reviewer's browser had the ESP-IDF
reset_reason.c page and arduino-esp32 issue 6762 open, and it built both variants in its own copy. It
made no tracked edits and touched no board.

## Resolutions (commit that adds this file)

Each finding was checked against the code and ESP-IDF v5.5 sources before fixing.

| # | finding | verdict | resolution |
|---|---|---|---|
| 1 | download and halt trips erase the trip record | confirmed: the guard edited CRC-covered fields, then called ESP-IDF's incrementing update, which resets a struct with a stale CRC | the CRC is recomputed first; the board test now reads the record back after download and halt trips |
| 2 | an invalid recovery image makes a new boot loop | confirmed: bootloader_utility.c resets on a failed explicit test load, no fallback scan | the guard verifies the test image (esp_image_verify) and falls back to download or halt; a pending-recovery bit in last_action also falls back when the recovery app booted at the previous trip and never marked itself healthy (a crash-looping recovery app, or anti-rollback refusing it) |
| 3 | panic hints are not guaranteed before a watchdog reset | confirmed: the panic handler arms its RTC watchdog before printing, and writes the hint after | documented as a coverage limit |
| 4 | recovery plus fast deep-sleep boot returns to the old app | confirmed by reading the load path | build-time #error for the combination |
| 5 | backup reuse without board provenance | confirmed (a run on the second board did keep the XIAO's backup file, without restoring it) | backups are named after the board MAC, written atomically, and restored only onto that board |
| 6 | PASS predicates overstate their evidence | confirmed for each listed case | reopen counts successful opens; halt lines must come from one boot; two-crash requires 1, 2, then 0; the watchdog check uses the count the app read at boot, before its healthy mark; restore requires boot lines; a lost ROM banner no longer fails a trip esptool confirmed |
| 7 | download recovery needs usable download mode and register access | accepted: eFuse-disabled or secure download mode defeats or limits it | documented |
| 8 | the PRO-CPU-only explanation is wrong for the S3 | confirmed: ESP-IDF limits it to the original dual-core ESP32 (esp_system Kconfig, memory-types.rst) | the core-0 restriction is removed; a spinlock guards the record |
| 9 | `guarded` can describe an earlier bootloader | confirmed | `guarded` now means the guard ran this boot (its hand-over flag was consumed by this app's constructor); the README states that an app without the component counts every esp_restart() |

The reviewer's full write-up (reset matrix, host reproductions) is kept with the review log, outside
the repository.

## FINDINGS

Tags: **(b)** an assumption holding in the demonstrated configuration; **(c)** a defect. No **(a)** arithmetic error found.

1. **[P2, c] Download and halt erase the trip record.**  
   The guard changes CRC-covered fields and then requests a validating, incrementing IDF update. IDF sees the stale CRC and resets the entire structure. Host execution reproduced `magic=0000, trips=0, last_action=0, reboot_counter=1` for both actions. The action still executes, so the measured trips remain credible; their diagnostic history disappears. [Guard:182](bootloader_components/main/bootguard_boot.c:182), [IDF update:239](esp-idf v5.5/components/bootloader_support/src/bootloader_common_loader.c:239).

2. **[P1, c] An invalid recovery image creates another boot loop.**  
   Recovery checks only whether `test.offset` exists. Explicit `TEST_APP_INDEX` loading resets immediately on invalid image or anti-rollback failure—there is no fallback scan. Because the guard already cleared its count, the next boot starts another sequence toward the same failed recovery attempt. The configured download/halt fallback covers an absent partition only. [Guard:94](bootloader_components/main/bootguard_boot.c:94), [IDF loader:585](esp-idf v5.5/components/bootloader_support/src/bootloader_utility.c:585).

3. **[P1 coverage limit, b] Panic hints are not guaranteed before watchdog reset.**  
   IDF arms the panic RWDT and disables the main watchdogs before printing/flushing/dumping; generic panic and IWDT hints are written afterward. A panic handler that stalls before that write can produce a hintless RTC reset after `loading` was cleared, which the guard ignores. Twenty such reset inputs remained at zero in the host harness. Task-WDT without panic can simply report and continue. [Panic setup:202](esp-idf v5.5/components/esp_system/port/panic_handler.c:202), [late hint:439](esp-idf v5.5/components/esp_system/panic.c:439), [classifier:83](bootloader_components/main/bootguard_boot.c:83).

4. **[P2, c] Recovery plus fast deep-sleep boot can return to the old app.**  
   Explicit TEST loading skips IDF’s retained-partition update. With `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`, a recovery app that sleeps can therefore wake into the previously cached application. That fast path runs before the guard. [IDF loader:507](esp-idf v5.5/components/bootloader_support/src/bootloader_utility.c:507), [bootloader:46](bootloader_components/main/bootloader_start.c:46).

5. **[P1, c] Backup reuse has no board or firmware provenance.**  
   An existing `backup.bin` is accepted solely by size. Switching boards or changing firmware between runs can restore stale or wrong-board data. The `finally` path also attempts restoration whenever the file exists, including a partial failed backup. The supplied log’s “kept existing” does not prove its backup was wrong, but does not establish its identity either. [board_test.py:118](tools/board_test.py:118), [254](tools/board_test.py:254).

6. **[P2, c] Several PASS predicates overstate their evidence.**  
   Synthetic inputs demonstrated acceptance of:
   - Five console opens that all failed.
   - Two halt messages from separate boots.
   - A healthy-clear claim with no observed nonzero count.
   - Restore with no console or running-image evidence.

   The watchdog-reset check observes the healthy app **after it clears the count**, masking an erroneous increment. Conversely, lost USB enumeration output can falsely fail a successful trip. These are weaknesses in the predicates, not proof that the supplied run failed. [Checks:148](tools/board_test.py:148), [healthy app:58](examples/crashloop/main/main.c:58).

7. **[P1 compatibility assumption, b] Download recovery requires suitable eFuses and register access.**  
   Disabled USB/download mode defeats the transport. Secure download disallows the register write used to clear OPTION1; esptool’s `hard_reset()` catches that failure and continues. Its normal exit workflow therefore is not unconditional. Exact flag retention across power/system-reset variants remains **unverified**, as does the unexplained XIAO exit. [IDF security configuration:1130](esp-idf v5.5/components/bootloader/Kconfig.projbuild:1130), [esptool:365](esptool/targets/esp32s3.py:365).

8. **[P3, c] The PRO-only hardware explanation is incorrect for S3.**  
   IDF limits that restriction to the original ESP32; S3 exposes remaining RTC FAST memory through its ordinary internal heap. Keeping this API restricted to core 0 can be a synchronization policy, but the README’s hardware justification is wrong. [IDF target condition:112](esp-idf v5.5/components/esp_system/Kconfig:112), [target-specific documentation:179](esp-idf v5.5/docs/en/api-guides/memory-types.rst:179).

9. **[P2, c] `guarded=true` can describe a previously installed bootloader.**  
   The API checks retained magic, not evidence that the guard ran this boot. A matching stock bootloader preserves that magic. Host execution reproduced `ESP_OK`, `guarded=true`, and an old reset reason after only IDF’s update ran. Conversely, an app missing the component’s constructor can trip after three entirely deliberate `esp_restart()` calls. The integration contract is necessary and is not automatically enforced across images. [Validity check:19](components/bootguard/bootguard.c:19), [API promise:24](components/bootguard/include/bootguard.h:24).

## SURVIVED

- **Both reported fixes work on normal paths.** Blank/corrupted retained memory survives subsequent image loading; `INVALID_INDEX` counts correctly. The remaining CRC defect is specifically the download/halt trip update.
- **The default layout matches:** structure at `0x600fffe8`, custom record at `0x600ffff4`; linker reservation excludes RTC heap and `.rtc_noinit`. Constructor linkage and startup ordering were verified.
- **The demonstrated fault paths survive:** abort, hintless TG1 stack overflow, IWDT, ordinary restarts, and healthy clearing have meaningful supplied board evidence.
- **The upstream override is faithful:** besides the include/call, differences are comments. No executable upstream behavior was removed.
- **Counter arithmetic, Kconfig propagation, build sizes, ordinary hint decoding, and wake-stub discrimination survive.**
- **Halt and unsecured download are supported for the tested setup.** Invalid TEST images still undergo validation and anti-rollback. BLE belongs in the recovery application; the excluded recovery work was not reviewed.
