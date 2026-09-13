/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 *
 * Application side of the boot guard.
 *
 * The guarded bootloader counts consecutive crash resets (panic, interrupt or
 * task watchdog, CPU lockup). The application declares a boot good by calling
 * bootguard_mark_healthy() once it has run long enough to trust, e.g. after
 * its main loop has completed a few seconds of work. Without that call, the
 * count keeps growing across crashes and the bootloader acts at the limit.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "bootguard_rec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool    guarded;      /* the guarded bootloader ran THIS boot (it set the hand-over flag this app's
                           * startup constructor consumed); false after a stock bootloader, even when an
                           * older record is still retained */
    uint8_t crashes;      /* consecutive crash resets, including the one that started this boot */
    uint8_t trips;        /* times the guard acted since the retained memory was last valid */
    uint8_t last_rom;     /* ROM reset reason of this boot */
    uint8_t last_hint;    /* ESP-IDF reset hint the bootloader read for this boot, 0 = none */
    uint8_t last_action;  /* bootguard_action_t of the latest trip */
    bool    recovery_pending; /* the recovery app booted at the latest trip and has not marked itself healthy */
} bootguard_status_t;

/* Read the record. ESP_ERR_NOT_FOUND: no valid record (an unguarded bootloader, or
 * the retained memory was lost); `out->guarded` is false. Callable from either core. */
esp_err_t bootguard_get_status(bootguard_status_t *out);

/* Clear the crash count and the pending-recovery flag. Callable from either core: the
 * PRO-CPU-only restriction on RTC FAST memory applies to the original ESP32, not the S3.
 * ESP_ERR_NOT_FOUND when there is no valid record.
 *
 * An application that uses the guarded bootloader must link this component: its startup
 * constructor clears the hand-over flag, and without it every hint-less esp_restart()
 * counts as a crash. */
esp_err_t bootguard_mark_healthy(void);

#ifdef __cplusplus
}
#endif
