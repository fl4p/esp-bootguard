/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 *
 * The record the boot guard keeps across resets. Shared by the second-stage
 * bootloader (which counts and acts) and the application (which reads it and
 * marks a boot healthy). It lives in the `custom` area of ESP-IDF's retained
 * RTC FAST memory (rtc_retain_mem_t), which survives every reset except a loss
 * of the RTC domain.
 *
 * ESP-IDF 5.x leaves the custom area OUT of that struct's CRC by default
 * (rtc_retain_mem_size() stops at `custom`), so the record would have no
 * integrity check at all. CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC puts it
 * back under the CRC: any change must then be followed by a CRC update, which
 * both sides do, and a corrupted or foreign record reads as invalid, so the
 * count starts over instead of acting on garbage.
 *
 * Both builds must see the same sdkconfig:
 *   CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
 *   CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x8   (or larger)
 *   CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC=y
 */
#pragma once
#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOTGUARD_MAGIC 0xB6A1

typedef enum {
    BOOTGUARD_ACTION_NONE     = 0,
    BOOTGUARD_ACTION_DOWNLOAD = 1,  /* set the ROM's force-download flag and reset */
    BOOTGUARD_ACTION_HALT     = 2,  /* park in the bootloader: no app, no reset loop */
    BOOTGUARD_ACTION_RECOVERY = 3,  /* boot the `test` app partition (e.g. a BLE OTA recovery app) */
} bootguard_action_t;

typedef struct {
    uint16_t magic;        /* BOOTGUARD_MAGIC once the guarded bootloader has run */
    uint8_t  crashes;      /* consecutive crash resets since the last healthy mark */
    uint8_t  trips;        /* times the guard acted since the retained memory was valid (saturating) */
    uint8_t  last_rom;     /* ROM reset reason of the latest boot (soc_reset_reason_t) */
    uint8_t  last_hint;    /* ESP-IDF reset hint of the latest boot (esp_reset_reason_t), 0 = none */
    uint8_t  last_action;  /* bootguard_action_t taken at the latest trip */
    uint8_t  loading;      /* set by the bootloader before it loads an image, cleared by the app
                            * component's startup constructor: a hint-less software or RTC-watchdog
                            * reset while set means the app never started */
} bootguard_rec_t;

#ifdef __cplusplus
static_assert(sizeof(bootguard_rec_t) == 8, "bootguard_rec_t layout");
#else
_Static_assert(sizeof(bootguard_rec_t) == 8, "bootguard_rec_t layout");
#endif

#if !CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
#error "boot guard: set CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y, CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x8 and CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC=y in sdkconfig"
#elif CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE < 8
#error "boot guard: CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE must be at least 0x8"
#elif !CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC
#error "boot guard: set CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC=y (without it ESP-IDF's CRC does not cover the record)"
#endif

#ifdef __cplusplus
}
#endif
