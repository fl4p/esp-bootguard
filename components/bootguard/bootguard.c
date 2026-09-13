/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bootguard.h"
#include "bootloader_common.h"
#include "freertos/FreeRTOS.h"
#include "esp_system.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_guarded_this_boot;

static bootguard_rec_t *rec(void)
{
    return (bootguard_rec_t *)bootloader_common_get_rtc_retain_mem()->custom;
}

/* ESP-IDF reports a reboot counter of 0 when the retained area's CRC fails;
 * a guarded bootloader has run whenever the magic is present. */
static bool rec_valid(void)
{
    return bootloader_common_get_rtc_retain_mem_reboot_counter() != 0 && rec()->magic == BOOTGUARD_MAGIC;
}

/* The app reached its constructors: whatever resets it from here on, the
 * bootloader's hand-over worked. Runs on core 0 before the scheduler starts. */
static void __attribute__((constructor)) bootguard_app_started(void)
{
    /* Force ESP-IDF's real reset-reason implementation to be linked.
     *
     * esp_reset_reason_set_hint() has a WEAK NO-OP definition in
     * esp_system/panic.c, and the strong one in
     * esp_system/port/soc/<target>/reset_reason.c is only pulled in when
     * something in the app references esp_reset_reason(). An app that never
     * calls it links the stub, so the panic handler's
     * esp_reset_reason_set_hint(ESP_RST_INT_WDT / TASK_WDT / PANIC) silently
     * does nothing and every panic reboots with no hint at all.
     *
     * The guard's whole crash test is built on that hint, so without this the
     * guard fails OPEN on the exact case it exists for: measured on an
     * ESP32-S3 (2026-09-13), 18 consecutive interrupt-watchdog panics all
     * logged "reset 0x0c hint 0: not counted, 0 of 3" and the board looped
     * until it was physically replugged. This component must therefore
     * guarantee its own precondition rather than assume the app does.
     *
     * The call itself is cheap and side-effect-free; the reference is the point. */
    (void)esp_reset_reason();

    if (rec_valid() && rec()->loading) {
        s_guarded_this_boot = true;   /* only the guarded bootloader sets the flag, on every hand-over */
        rec()->loading = 0;
        bootloader_common_update_rtc_retain_mem(NULL, false);
    }
}

esp_err_t bootguard_get_status(bootguard_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (bootguard_status_t){0};
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (!rec_valid()) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        const bootguard_rec_t *r = rec();
        out->guarded = s_guarded_this_boot;
        out->crashes = r->crashes;
        out->trips = r->trips;
        out->last_rom = r->last_rom;
        out->last_hint = r->last_hint;
        out->last_action = r->last_action & (uint8_t)~BOOTGUARD_RECOVERY_PENDING;
        out->recovery_pending = (r->last_action & BOOTGUARD_RECOVERY_PENDING) != 0;
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t bootguard_mark_healthy(void)
{
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (!rec_valid()) {
        err = ESP_ERR_NOT_FOUND;
    } else if (rec()->crashes != 0 || (rec()->last_action & BOOTGUARD_RECOVERY_PENDING)) {
        rec()->crashes = 0;
        rec()->last_action &= (uint8_t)~BOOTGUARD_RECOVERY_PENDING;
        bootloader_common_update_rtc_retain_mem(NULL, false);   /* recompute the CRC only */
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}
