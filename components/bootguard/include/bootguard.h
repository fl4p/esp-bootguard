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
    bool    guarded;      /* the running bootloader is the guarded one (the record carries its magic) */
    uint8_t crashes;      /* consecutive crash resets, including the one that started this boot */
    uint8_t trips;        /* times the guard acted since the retained memory was last valid */
    uint8_t last_rom;     /* ROM reset reason of this boot */
    uint8_t last_hint;    /* ESP-IDF reset hint the bootloader read for this boot, 0 = none */
    uint8_t last_action;  /* bootguard_action_t of the latest trip */
} bootguard_status_t;

/* Read the record. ESP_ERR_NOT_FOUND: no valid record (an unguarded
 * bootloader, or the retained memory was lost); `out->guarded` is false. */
esp_err_t bootguard_get_status(bootguard_status_t *out);

/* Clear the crash count. Call from core 0: ESP-IDF's retained RTC FAST memory
 * is only reachable from the PRO CPU (ESP_ERR_INVALID_STATE otherwise).
 * ESP_ERR_NOT_FOUND when the bootloader is not the guarded one. */
esp_err_t bootguard_mark_healthy(void);

#ifdef __cplusplus
}
#endif
