/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_radio.h - Thin ESP-IDF glue shared by the node and the gateway:
 * Wi-Fi bring-up for ESP-NOW, channel/LR/TX-power setup, PMK/LMK handling
 * and peer management. Callbacks run in the Wi-Fi task: keep them short.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mesh_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the Wi-Fi task for every received ESP-NOW frame. Must not block. */
typedef void (*mesh_radio_recv_cb_t)(const uint8_t src_mac[MESH_MAC_LEN], const uint8_t *data, int len,
                                     int8_t rssi, bool broadcast);

/** Called from the Wi-Fi task when the MAC layer finished a transmission. */
typedef void (*mesh_radio_send_cb_t)(const uint8_t dst_mac[MESH_MAC_LEN], bool delivered);

/** Initialise NVS-independent Wi-Fi in STA mode, ESP-NOW, PMK and callbacks. */
esp_err_t mesh_radio_init(mesh_radio_recv_cb_t on_recv, mesh_radio_send_cb_t on_send);

/**
 * Add (or update) a unicast peer. When encryption is enabled in Kconfig and
 * @p encrypt is true the peer uses the configured LMK.
 */
esp_err_t mesh_radio_add_peer(const uint8_t mac[MESH_MAC_LEN], bool encrypt);
esp_err_t mesh_radio_del_peer(const uint8_t mac[MESH_MAC_LEN]);
bool mesh_radio_has_peer(const uint8_t mac[MESH_MAC_LEN]);

/** Queue a frame for transmission. Completion is reported via the send callback. */
esp_err_t mesh_radio_send(const uint8_t mac[MESH_MAC_LEN], const uint8_t *data, size_t len);

bool mesh_radio_encryption_enabled(void);
uint8_t mesh_radio_channel(void);
void mesh_radio_get_mac(uint8_t mac[MESH_MAC_LEN]);

extern const uint8_t MESH_BROADCAST_MAC[MESH_MAC_LEN];

#ifdef __cplusplus
}
#endif
