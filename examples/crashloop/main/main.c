/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercise the boot guard: print the guard's record, then crash, restart, or
 * run healthily, depending on the CONFIG_CRASHLOOP_KIND_* variant built.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "bootguard.h"
#include "bootloader_common.h"
#include "sdkconfig.h"

static const char *action_name(uint8_t a)
{
    switch (a) {
    case BOOTGUARD_ACTION_NONE:     return "none";
    case BOOTGUARD_ACTION_DOWNLOAD: return "download";
    case BOOTGUARD_ACTION_HALT:     return "halt";
    case BOOTGUARD_ACTION_RECOVERY: return "recovery";
    default:                        return "?";
    }
}

#if CONFIG_CRASHLOOP_KIND_TWO_CRASHES_THEN_HEALTHY
#include "esp_attr.h"
static RTC_NOINIT_ATTR uint32_t s_restarted;   /* survives the deliberate restart */
#endif

#if CONFIG_CRASHLOOP_KIND_STACK_OVERFLOW
static volatile int s_depth_limit = 1 << 30;   /* volatile: GCC must not prove the recursion infinite (-Werror=infinite-recursion) */

static __attribute__((noinline)) int blow_stack(int depth)
{
    volatile char pad[512];
    memset((char *)pad, depth & 0xff, sizeof pad);
    if (depth >= s_depth_limit) {
        return 0;
    }
    return blow_stack(depth + 1) + pad[depth % sizeof pad];   /* not a tail call */
}
#endif

void app_main(void)
{
    bootguard_status_t st;
    esp_err_t err = bootguard_get_status(&st);
    printf("crashloop: esp_reset_reason %d; guard %s: crashes %u trips %u last_rom 0x%02x last_hint %u last_action %s\n",
           (int)esp_reset_reason(), err == ESP_OK ? "on" : esp_err_to_name(err),
           st.crashes, st.trips, st.last_rom, st.last_hint, action_name(st.last_action));

    vTaskDelay(pdMS_TO_TICKS(CONFIG_CRASHLOOP_DELAY_MS));

#if CONFIG_CRASHLOOP_KIND_HEALTHY
    err = bootguard_mark_healthy();
    printf("crashloop: marked healthy (%s)\n", esp_err_to_name(err));
    for (int n = 0;; n++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        /* repeat the boot's state: the lines printed right after a reset can be
         * lost on the host while the USB device re-enumerates */
        const bootguard_rec_t *raw = (const bootguard_rec_t *)bootloader_common_get_rtc_retain_mem()->custom;
        err = bootguard_get_status(&st);
        printf("crashloop: alive %d; esp_reset_reason %d; status %s crashes %u; reboot_counter %u; raw magic 0x%04x crashes %u trips %u last_rom 0x%02x last_hint %u loading %u\n",
               n, (int)esp_reset_reason(), esp_err_to_name(err), st.crashes,
               bootloader_common_get_rtc_retain_mem_reboot_counter(),
               raw->magic, raw->crashes, raw->trips, raw->last_rom, raw->last_hint, raw->loading);
    }
#elif CONFIG_CRASHLOOP_KIND_ABORT
    printf("crashloop: abort()\n");
    abort();
#elif CONFIG_CRASHLOOP_KIND_STACK_OVERFLOW
    printf("crashloop: overflowing the stack\n");
    printf("%d\n", blow_stack(0));
#elif CONFIG_CRASHLOOP_KIND_INT_WDT
    printf("crashloop: interrupts off, spinning\n");
    portDISABLE_INTERRUPTS();
    for (;;) {
    }
#elif CONFIG_CRASHLOOP_KIND_RESTART
    printf("crashloop: esp_restart()\n");
    esp_restart();
#elif CONFIG_CRASHLOOP_KIND_TWO_CRASHES_THEN_HEALTHY
    const esp_reset_reason_t why = esp_reset_reason();
    if (why == ESP_RST_POWERON || why == ESP_RST_USB) {
        s_restarted = 0;
    }
    if (st.crashes < 2 && s_restarted == 0) {
        printf("crashloop: abort() at count %u\n", st.crashes);
        abort();
    }
    if (s_restarted == 0) {
        err = bootguard_mark_healthy();
        printf("crashloop: marked healthy (%s), restarting once\n", esp_err_to_name(err));
        s_restarted = 1;
        esp_restart();
    }
    printf("crashloop: after the deliberate restart the count is %u\n", st.crashes);
    for (int n = 0;; n++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        printf("crashloop: alive %d\n", n);
    }
#endif
}
