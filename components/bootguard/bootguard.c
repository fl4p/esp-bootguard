/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bootguard.h"
#include "bootloader_common.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

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
    if (esp_cpu_get_core_id() == 0 && rec_valid() && rec()->loading) {
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
    if (esp_cpu_get_core_id() != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rec_valid()) {
        return ESP_ERR_NOT_FOUND;
    }
    const bootguard_rec_t *r = rec();
    out->guarded = true;
    out->crashes = r->crashes;
    out->trips = r->trips;
    out->last_rom = r->last_rom;
    out->last_hint = r->last_hint;
    out->last_action = r->last_action;
    return ESP_OK;
}

esp_err_t bootguard_mark_healthy(void)
{
    if (esp_cpu_get_core_id() != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (!rec_valid()) {
        err = ESP_ERR_NOT_FOUND;
    } else if (rec()->crashes != 0) {
        rec()->crashes = 0;
        bootloader_common_update_rtc_retain_mem(NULL, false);   /* recompute the CRC only */
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}
