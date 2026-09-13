/*
 * SPDX-FileCopyrightText: 2026 Fabian Schlieper
 * SPDX-License-Identifier: Apache-2.0
 *
 * The GATT layer esp-ota-ble's host tools expect, on the NimBLE host. It matches the one in
 * fugu-mppt-firmware (src/tele/console_ble.cpp) so fugu's etc/ota_ble.py pushes unchanged:
 *
 *   service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (Nordic UART), advertised
 *     RX 6E400002  write / write-without-response   command lines, '\n' terminated
 *     TX 6E400003  notify                           status lines, a byte stream chunked at MTU-3
 *     FW 6E400004  write-without-response           firmware bytes
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented by the application. All three run on the NimBLE host task and must not block. */
void nus_on_line(const char *line);                  /* one RX line, without the terminator */
void nus_on_fw(const uint8_t *data, size_t len);     /* FW bytes */
void nus_on_disconnect(void);

/* Bring up NVS, the controller, the host task and advertising. Returns 0 on success. */
int nus_start(const char *name);

/* Queue bytes for the TX characteristic; dropped when no client is subscribed or the queue is
 * full. Any task. */
void nus_send(const char *data, size_t len);

/* Send queued TX bytes as far as NimBLE's buffers allow, and issue the deferred connection
 * parameter request. Call from an ordinary task, never from the host task. */
void nus_drain(void);

bool nus_connected(void);

#ifdef __cplusplus
}
#endif
