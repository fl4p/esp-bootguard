/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot guard, bootloader side: count consecutive crash resets and act at a
 * limit (ROM download mode, halt, or a recovery partition).
 *
 * What counts as a crash, from the ROM reset reason plus the hint ESP-IDF's
 * panic handler writes before it resets (esp_system/port/soc/esp32s3/reset_reason.c):
 *   - a hint of PANIC, INT_WDT, TASK_WDT, WDT or CPU_LOCKUP;
 *   - a main-watchdog reset without a hint: the panic handler itself hung, or never
 *     ran (measured on an S3: a main-task stack overflow reset through the timer
 *     group 1 watchdog, rst:0x8, with no hint and no panic output);
 *   - a software or RTC-watchdog reset without a hint while `loading` is still
 *     set: the bootloader handed over to an image but the app never reached
 *     its constructors (a load failure, or a hang the bootloader's RTC watchdog
 *     caught). A plain esp_restart() writes no hint either, but by then the
 *     app has cleared `loading`, so it does not count.
 * A power-on, brownout or super-watchdog reset (one ROM code), and a reset
 * from the USB host (USB-UART / USB-JTAG), clear the count. Everything else
 * leaves it unchanged; notably esptool's --after watchdog_reset.
 */
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/reset_reasons.h"
#include "hal/wdt_hal.h"
#include "bootloader_common.h"
#include "esp_image_format.h"
#include "bootguard_boot.h"
#include "bootguard_rec.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "boot guard: only the ESP32-S3 is implemented (the reset-hint and force-download registers are target specific)"
#endif

#if CONFIG_BOOTGUARD_ACTION_RECOVERY && CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
/* ESP-IDF's fast deep-sleep path runs before the guard and boots the partition cached by the last
 * normal load; an explicit test-partition load does not update that cache, so a recovery app that
 * sleeps would wake into the crashing app (review of fb6a673, finding 4) */
#error "boot guard: CONFIG_BOOTGUARD_ACTION_RECOVERY cannot be combined with CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP"
#endif

#if CONFIG_BOOTGUARD_ENABLE

static const char *TAG = "bootguard";

/* esp_reset_reason_t, mirrored: esp_system.h is not part of the bootloader build */
enum {
    HINT_NONE = 0, HINT_POWERON, HINT_EXT, HINT_SW, HINT_PANIC, HINT_INT_WDT, HINT_TASK_WDT, HINT_WDT,
    HINT_DEEPSLEEP, HINT_BROWNOUT, HINT_SDIO, HINT_USB, HINT_JTAG, HINT_EFUSE, HINT_PWR_GLITCH, HINT_CPU_LOCKUP,
};

/* The hint register format of esp_reset_reason_set_hint(): value in bits 0..14,
 * a copy in bits 16..30, bit 31 set. Anything else (a deep-sleep wake stub
 * address, zero after the app cleared it) reads as no hint. */
static uint8_t read_hint(void)
{
    const uint32_t v = REG_READ(RTC_CNTL_STORE6_REG);
    const uint32_t lo = v & 0x7FFF, hi = (v >> 16) & 0x7FFF;
    if ((v & 0x80000000u) == 0 || lo != hi || lo > 0xFF) {
        return HINT_NONE;
    }
    return (uint8_t)lo;
}

static bool is_host_or_power_reset(soc_reset_reason_t rom)
{
    return rom == RESET_REASON_CHIP_POWER_ON      /* also brownout and super watchdog: same code */
        || rom == RESET_REASON_CORE_USB_UART
        || rom == RESET_REASON_CORE_USB_JTAG;
}

static bool is_crash(soc_reset_reason_t rom, uint8_t hint, bool loading)
{
    switch (hint) {
    case HINT_PANIC: case HINT_INT_WDT: case HINT_TASK_WDT: case HINT_WDT: case HINT_CPU_LOCKUP:
        return true;
    case HINT_NONE:
        break;
    default:
        return false;
    }
    switch (rom) {
    case RESET_REASON_CORE_MWDT0: case RESET_REASON_CORE_MWDT1:
    case RESET_REASON_CPU0_MWDT0: case RESET_REASON_CPU0_MWDT1:
        return true;
    case RESET_REASON_CORE_SW: case RESET_REASON_CPU0_SW:
    case RESET_REASON_CORE_RTC_WDT: case RESET_REASON_CPU0_RTC_WDT: case RESET_REASON_SYS_RTC_WDT:
        return loading;
    default:
        return false;
    }
}

static bootguard_action_t fallback_action(void)
{
#if CONFIG_BOOTGUARD_ACTION_HALT || CONFIG_BOOTGUARD_RECOVERY_FALLBACK_HALT
    return BOOTGUARD_ACTION_HALT;
#else
    return BOOTGUARD_ACTION_DOWNLOAD;
#endif
}

static bootguard_action_t configured_action(const bootloader_state_t *bs, const bootguard_rec_t *r)
{
#if CONFIG_BOOTGUARD_ACTION_RECOVERY
    /* An explicit test-partition load that fails resets without ESP-IDF's fallback scan, and the
     * count was just cleared, so choosing recovery blindly turns a bad recovery image into a slower
     * endless loop (review of fb6a673, finding 2). Fall back when there is no test partition, when
     * it holds no valid image, or when the recovery app booted at the previous trip and never
     * marked itself healthy (a crash-looping recovery app, or an image anti-rollback refused). */
    if (bs->test.offset == 0) {
        ESP_LOGE(TAG, "no test partition for the recovery action");
        return fallback_action();
    }
    if (r->last_action & BOOTGUARD_RECOVERY_PENDING) {
        ESP_LOGE(TAG, "the recovery app booted at the previous trip and never marked itself healthy");
        return fallback_action();
    }
    esp_image_metadata_t md;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &bs->test, &md) != ESP_OK) {
        ESP_LOGE(TAG, "the test partition holds no valid image");
        return fallback_action();
    }
    return BOOTGUARD_ACTION_RECOVERY;
#else
    (void)bs;
    (void)r;
    return fallback_action();
#endif
}

/* The bootloader arms the RTC watchdog (CONFIG_BOOTLOADER_WDT_TIME_MS) for the rest of its own
 * run. Nothing feeds it in ROM download mode or in the halt loop, and its reset also resets the
 * RTC domain, clearing the force-download flag: measured on an S3, an unattended chip left
 * download mode 9.7 s after every trip and started the crash cycle again. Disable it first. */
static void rtc_wdt_disable(void)
{
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_disable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);
}

static void __attribute__((noreturn)) enter_download_mode(void)
{
    ESP_LOGE(TAG, "entering ROM download mode; esptool --after hard_reset (or a power cycle) leaves it");
    esp_rom_delay_us(100 * 1000);   /* let the log drain from the USB FIFO */
    rtc_wdt_disable();
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
    while (true) {
    }
}

static void __attribute__((noreturn)) halt(void)
{
    rtc_wdt_disable();
    for (unsigned n = 0;; n++) {
        ESP_LOGE(TAG, "halted after %d consecutive crash resets (%u); reflash over USB", CONFIG_BOOTGUARD_MAX_CRASHES, n);
        for (int i = 0; i < 50; i++) {
            esp_rom_delay_us(100 * 1000);
        }
    }
}

int bootguard_check(const bootloader_state_t *bs, int boot_index)
{
    /* ESP-IDF reports a reboot counter of 0 when the retained area's CRC fails.
     * A plain reset is not enough: a zeroed struct has a CRC of exactly
     * UINT32_MAX (the ROM crc32_le of zero bytes with a UINT32_MAX seed), which
     * ESP-IDF's validity check rejects, so its own update at image load would
     * reset the struct again and wipe the record written below (measured on an
     * S3: the first boot after a foreign image or a corrupted CRC lost the
     * record). Let ESP-IDF's update establish a valid struct instead; its
     * reboot counter then reads 2 on that first boot rather than 1. */
    if (bootloader_common_get_rtc_retain_mem_reboot_counter() == 0) {
        bootloader_common_reset_rtc_retain_mem();
        bootloader_common_update_rtc_retain_mem(NULL, true);
    }
    bootguard_rec_t *r = (bootguard_rec_t *)bootloader_common_get_rtc_retain_mem()->custom;
    if (r->magic != BOOTGUARD_MAGIC) {
        memset(r, 0, sizeof(*r));
        r->magic = BOOTGUARD_MAGIC;
    }

    const soc_reset_reason_t rom = esp_rom_get_reset_reason(0);
    const uint8_t hint = read_hint();
    const bool loading = r->loading != 0;
    r->last_rom = (uint8_t)rom;
    r->last_hint = hint;

    const bool host = is_host_or_power_reset(rom);
    const bool crash = !host && is_crash(rom, hint, loading);
    const bool no_app = boot_index == INVALID_INDEX;
    if (host) {
        r->crashes = 0;
    }
    if ((crash || no_app) && r->crashes < UINT8_MAX) {
        r->crashes++;
    }
    ESP_LOGI(TAG, "reset 0x%02x hint %u%s%s: %s, %u of %d",
             (unsigned)rom, (unsigned)hint, loading ? " app-not-started" : "", no_app ? " no-bootable-app" : "",
             host ? "count cleared" : ((crash || no_app) ? "counted" : "not counted"),
             (unsigned)r->crashes, CONFIG_BOOTGUARD_MAX_CRASHES);

    if (r->crashes < CONFIG_BOOTGUARD_MAX_CRASHES) {
        r->loading = !no_app;   /* cleared by the app component's constructor */
        bootloader_common_update_rtc_retain_mem(NULL, false);
        return boot_index;
    }

    const bootguard_action_t action = configured_action(bs, r);
    r->crashes = 0;   /* the image flashed in recovery gets a full set of tries */
    if (r->trips < UINT8_MAX) {
        r->trips++;
    }
    r->last_action = (uint8_t)action | (action == BOOTGUARD_ACTION_RECOVERY ? BOOTGUARD_RECOVERY_PENDING : 0);
    r->loading = action == BOOTGUARD_ACTION_RECOVERY;
    bootloader_common_update_rtc_retain_mem(NULL, false);   /* the CRC now covers the new record */
    if (action != BOOTGUARD_ACTION_RECOVERY) {
        /* No image load follows download or halt, so count this pass here. The CRC must be valid
         * first: ESP-IDF's incrementing update resets a struct whose CRC is stale, which erased the
         * trip record (review of fb6a673, finding 1). */
        bootloader_common_update_rtc_retain_mem(NULL, true);
    }
    ESP_LOGE(TAG, "%d consecutive crash resets", CONFIG_BOOTGUARD_MAX_CRASHES);

    switch (action) {
    case BOOTGUARD_ACTION_RECOVERY:
        ESP_LOGW(TAG, "booting the test partition (recovery)");
        return TEST_APP_INDEX;
    case BOOTGUARD_ACTION_HALT:
        halt();
    default:
        enter_download_mode();
    }
}

#else /* !CONFIG_BOOTGUARD_ENABLE */

int bootguard_check(const bootloader_state_t *bs, int boot_index)
{
    (void)bs;
    return boot_index;
}

#endif
