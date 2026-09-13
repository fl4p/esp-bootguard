/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot guard recovery app. The guarded bootloader boots it from the `test` partition after the
 * crash limit. It serves the Nordic-UART GATT layer esp-ota-ble's host tools speak, accepts an
 * image over BLE, writes it to ota_0 (esp_ota_get_next_update_partition() from `test`), selects
 * that slot and reboots into it.
 */
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "bootguard.h"
#include "ota_ble.h"
#include "ble_nus.h"

static const char *TAG = "recovery";
static TaskHandle_t s_loop_task;

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

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

static void reply(const char *s)
{
    nus_send(s, strlen(s));
}

/* esp-ota-ble status hook (consumer task, and synchronously from otaBleSubmitCommand on the host
 * task). Severity is preserved; the per-window CRED/PROG chatter goes to DEBUG so the console on an
 * unread USB-Serial-JTAG never sits in the transfer's path. */
static void status_hook(OtaBleLevel level, const char *line)
{
    nus_send(line, strlen(line));
    nus_send("\n", 1);
    switch (level) {
    case OtaBleLevel::Info:
        if (strncmp(line, "OTAB CRED", 9) == 0 || strncmp(line, "OTAB PROG", 9) == 0) {
            ESP_LOGD(TAG, "%s", line);
        } else {
            ESP_LOGI(TAG, "%s", line);
        }
        break;
    case OtaBleLevel::Warn:  ESP_LOGW(TAG, "%s", line); break;
    case OtaBleLevel::Error: ESP_LOGE(TAG, "%s", line); break;
    }
}

/* `end` verified the image and selected ota_0: let `OTAB OK` drain before the reset. */
static void restart_hook()
{
    for (int i = 0; i < 30 && nus_connected(); i++) {
        nus_drain();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "restarting into the new image");
    esp_restart();
}

static bool is_ota_command(const char *p)
{
    static const char *const words[] = { "begin", "resume", "end", "abort", "info" };
    for (const char *w : words) {
        size_t n = strlen(w);
        if (strncmp(p, w, n) == 0 && (p[n] == '\0' || p[n] == ' ')) {
            return true;
        }
    }
    return false;
}

/* RX line, host task. Accepts fugu's console form ("ota-ble begin ...", etc/ota_ble.py passes
 * cmd_prefix="ota-ble ") and the bare form esp_ota_ble.push_image() sends by default. */
extern "C" void nus_on_line(const char *line)
{
    const char *p = line;
    while (*p == ' ') {
        p++;
    }
    if (strncmp(p, "ota-ble ", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "otab ", 5) == 0) {
        p += 5;
    }
    if (is_ota_command(p)) {
        if (otaBleSubmitCommand(p) == OtaBleSubmit::Rejected) {
            reply("ERR: otab: expected begin <size> <sha256hex> | resume | end | abort | info\n");
        }
    } else if (strcmp(p, "ping") == 0) {
        reply("pong\n");
    } else {
        /* no `uptime` App: line on purpose: fugu's tool then never skips the push as "same version" */
        reply("ERR: recovery app; commands: ota-ble begin|resume|end|abort|info, ping\n");
    }
}

/* FW bytes, host task: copy only. */
extern "C" void nus_on_fw(const uint8_t *data, size_t len)
{
    otaBleStageBytes(data, len);
    if (s_loop_task) {
        xTaskNotifyGive(s_loop_task);
    }
}

extern "C" void nus_on_disconnect(void)
{
    otaBleRequestAbort();
}

extern "C" void app_main(void)
{
    s_loop_task = xTaskGetCurrentTaskHandle();

    bootguard_status_t st = {};
    esp_err_t err = bootguard_get_status(&st);
    ESP_LOGI(TAG, "reset reason %d; guard %s: crashes %u trips %u last_rom 0x%02x last_hint %u last_action %s",
             (int)esp_reset_reason(), err == ESP_OK ? "on" : esp_err_to_name(err),
             st.crashes, st.trips, st.last_rom, st.last_hint, action_name(st.last_action));

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    ESP_LOGI(TAG, "running from %s @0x%" PRIx32 "; update target %s @0x%" PRIx32 " size 0x%" PRIx32,
             run ? run->label : "?", run ? run->address : 0,
             next ? next->label : "none", next ? next->address : 0, next ? next->size : 0);
    if (next == nullptr || next == run) {
        ESP_LOGE(TAG, "no update slot distinct from the running partition: a push will be refused");
    }

    OtaBleHooks hooks;
    hooks.status = status_hook;
    hooks.restart = restart_hook;
    otaBleInit(hooks);

    if (nus_start(CONFIG_RECOVERY_BLE_NAME) != 0) {
        ESP_LOGE(TAG, "BLE start failed");
    }

    bool marked = CONFIG_RECOVERY_HEALTHY_AFTER_MS == 0;
    for (;;) {
        /* Tick rate is the transfer rate: wake on staged bytes, otherwise every 10 ms. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        otaBleTick(now_ms());
        nus_drain();
        if (!marked && now_ms() >= (uint32_t)CONFIG_RECOVERY_HEALTHY_AFTER_MS) {
            marked = true;   /* app_main runs on core 0, where the retained memory is reachable */
            ESP_LOGI(TAG, "bootguard_mark_healthy: %s", esp_err_to_name(bootguard_mark_healthy()));
        }
    }
}
