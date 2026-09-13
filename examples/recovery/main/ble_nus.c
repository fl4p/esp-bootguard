/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "ble_nus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "nus";

/* 6E40000x-B5A3-F393-E0A9-E50E24DCCA9E, little-endian */
#define NUS_UUID(x) BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                                     0x93, 0xf3, 0xa3, 0xb5, (x), 0x00, 0x40, 0x6e)
static const ble_uuid128_t s_svc_uuid = NUS_UUID(0x01);
static const ble_uuid128_t s_rx_uuid  = NUS_UUID(0x02);
static const ble_uuid128_t s_tx_uuid  = NUS_UUID(0x03);
static const ble_uuid128_t s_fw_uuid  = NUS_UUID(0x04);

static uint16_t s_tx_handle;
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile bool s_params_pending;
static int64_t s_connect_us;

/* TX queue: status lines are tens of bytes; 4 KB covers several credit windows of backlog. */
#define TX_CAP 4096
static char s_tx[TX_CAP];
static size_t s_tx_len;
static SemaphoreHandle_t s_tx_mtx;

/* RX line assembly, host task only */
static char s_line[256];
static size_t s_line_len;
static bool s_line_overflow;

static void advertise(void);

static void tx_clear(void)
{
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    s_tx_len = 0;
    xSemaphoreGive(s_tx_mtx);
}

void nus_send(const char *data, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return;
    }
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    if (len > TX_CAP - s_tx_len) {
        len = TX_CAP - s_tx_len;   /* drop the overflow rather than block a producer */
    }
    memcpy(s_tx + s_tx_len, data, len);
    s_tx_len += len;
    xSemaphoreGive(s_tx_mtx);
}

bool nus_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void nus_drain(void)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (s_params_pending && esp_timer_get_time() - s_connect_us > 500000) {
        /* Deferred off the connect callback (fugu: a request from there tripped a controller
         * assert). 7.5..15 ms interval, latency 0, 4 s supervision timeout. Best effort. */
        s_params_pending = false;
        struct ble_gap_upd_params p = {
            .itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400,
            .min_ce_len = 0, .max_ce_len = 0,
        };
        int rc = ble_gap_update_params(conn, &p);
        ESP_LOGI(TAG, "connection parameter request rc=%d", rc);
    }
    if (!s_subscribed) {
        return;
    }
    uint16_t mtu = ble_att_mtu(conn);
    size_t chunk = mtu > 23 ? (size_t)mtu - 3 : 20;
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    while (s_tx_len > 0) {
        /* An exhausted mbuf pool mid-send can assert inside NimBLE; leave headroom (fugu). */
        if (os_msys_num_free() < 8) {
            break;
        }
        size_t n = s_tx_len < chunk ? s_tx_len : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_tx, (uint16_t)n);
        if (om == NULL) {
            break;
        }
        if (ble_gatts_notify_custom(conn, s_tx_handle, om) != 0) {
            break;   /* om is consumed either way; the bytes stay queued for the next call */
        }
        memmove(s_tx, s_tx + n, s_tx_len - n);
        s_tx_len -= n;
    }
    xSemaphoreGive(s_tx_mtx);
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const bool fw = arg != NULL;
    for (struct os_mbuf *m = ctxt->om; m != NULL; m = SLIST_NEXT(m, om_next)) {
        if (fw) {
            nus_on_fw(m->om_data, m->om_len);
            continue;
        }
        for (uint16_t i = 0; i < m->om_len; i++) {
            char c = (char)m->om_data[i];
            if (c == '\n' || c == '\r') {
                if (s_line_len > 0 && !s_line_overflow) {
                    s_line[s_line_len] = '\0';
                    nus_on_line(s_line);
                }
                s_line_len = 0;
                s_line_overflow = false;
            } else if (s_line_len < sizeof s_line - 1) {
                s_line[s_line_len++] = c;
            } else {
                s_line_overflow = true;
            }
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_fw_uuid.u,
                .access_cb = chr_access,
                .arg = (void *)1,   /* non-NULL marks the firmware sink */
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (event->connect.status == 0) {
            s_subscribed = false;
            s_line_len = 0;
            s_line_overflow = false;
            tx_clear();
            s_connect_us = esp_timer_get_time();
            s_params_pending = true;
            s_conn = event->connect.conn_handle;
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_params_pending = false;
        tx_clear();
        /* unconditional: also cancels a begin latched but not yet executed */
        nus_on_disconnect();
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "TX notify %s", s_subscribed ? "on" : "off");
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %u", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &s_svc_uuid;   /* flags 3 + uuid 18 = 21 of 31 bytes */
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp = { 0 };
    const char *name = ble_svc_gap_device_name();
    rsp.name = (const uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "no BLE address rc=%d", rc);
        return;
    }
    uint8_t a[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    ESP_LOGI(TAG, "advertising as '%s' %02x:%02x:%02x:%02x:%02x:%02x",
             ble_svc_gap_device_name(), a[5], a[4], a[3], a[2], a[1], a[0]);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int nus_start(const char *name)
{
    s_tx_mtx = xSemaphoreCreateMutex();
    if (s_tx_mtx == NULL) {
        return -1;
    }
    esp_err_t err = nvs_flash_init();   /* PHY calibration data */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s (continuing)", esp_err_to_name(err));
    }
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return -1;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(name);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup rc=%d", rc);
        return rc;
    }
    nimble_port_freertos_init(host_task);
    return 0;
}
