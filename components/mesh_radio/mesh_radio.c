/*
 * SPDX-License-Identifier: MIT
 */
#include "mesh_radio.h"

#include <string.h>

#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "mesh_radio";

#if CONFIG_MESH_LONG_RANGE
#define MESH_LR_ENABLED 1
#else
#define MESH_LR_ENABLED 0
#endif

const uint8_t MESH_BROADCAST_MAC[MESH_MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static mesh_radio_recv_cb_t s_on_recv;
static mesh_radio_send_cb_t s_on_send;
static uint8_t s_lmk[ESP_NOW_KEY_LEN];

static void recv_trampoline(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (s_on_recv == NULL || info == NULL || data == NULL || len <= 0) {
        return;
    }
    const int8_t rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;
    const bool bcast = (info->des_addr != NULL) && mesh_mac_is_broadcast(info->des_addr);
    s_on_recv(info->src_addr, data, len, rssi, bcast);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void send_trampoline(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (s_on_send != NULL && tx_info != NULL) {
        s_on_send(tx_info->des_addr, status == ESP_NOW_SEND_SUCCESS);
    }
}
#else
static void send_trampoline(const uint8_t *mac, esp_now_send_status_t status)
{
    if (s_on_send != NULL && mac != NULL) {
        s_on_send(mac, status == ESP_NOW_SEND_SUCCESS);
    }
}
#endif

bool mesh_radio_encryption_enabled(void)
{
#if CONFIG_MESH_ENCRYPTION
    return true;
#else
    return false;
#endif
}

uint8_t mesh_radio_channel(void)
{
    return (uint8_t)CONFIG_MESH_WIFI_CHANNEL;
}

void mesh_radio_get_mac(uint8_t mac[MESH_MAC_LEN])
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

/* Undo a partial bring-up so that mesh_radio_init() can be retried. */
static void radio_teardown(void)
{
    (void)esp_now_deinit();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
}

esp_err_t mesh_radio_init(mesh_radio_recv_cb_t on_recv, mesh_radio_send_cb_t on_send)
{
    s_on_recv = on_recv;
    s_on_send = on_send;

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE: already created */
        return err;
    }
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err == ESP_OK) {
        /* ESP-NOW needs no credentials: keep Wi-Fi configuration out of NVS. */
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        /* Modem sleep would make the radio doze between beacons and miss frames. */
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_channel(CONFIG_MESH_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
#if CONFIG_MESH_LONG_RANGE
    if (err == ESP_OK) {
        err = esp_wifi_set_protocol(WIFI_IF_STA,
                                    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    }
#endif
    if (err == ESP_OK) {
        err = esp_wifi_set_max_tx_power(CONFIG_MESH_TX_POWER_QDBM);
    }
    if (err == ESP_OK) {
        err = esp_now_init();
    }
    if (err == ESP_OK) {
        err = esp_now_register_recv_cb(recv_trampoline);
    }
    if (err == ESP_OK) {
        err = esp_now_register_send_cb(send_trampoline);
    }
#if CONFIG_MESH_ENCRYPTION
    if (err == ESP_OK) {
        uint8_t pmk[ESP_NOW_KEY_LEN];
        if (!mesh_parse_hex(CONFIG_MESH_PMK, pmk, sizeof(pmk)) ||
            !mesh_parse_hex(CONFIG_MESH_LMK, s_lmk, sizeof(s_lmk))) {
            ESP_LOGE(TAG, "MESH_PMK / MESH_LMK must be exactly 32 hex characters");
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = esp_now_set_pmk(pmk);
        }
        memset(pmk, 0, sizeof(pmk));
    }
#else
    memset(s_lmk, 0, sizeof(s_lmk));
#endif
    if (err == ESP_OK) {
        /* Broadcast peer (never encrypted) for JOIN discovery. */
        err = mesh_radio_add_peer(MESH_BROADCAST_MAC, false);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio bring-up failed: %s", esp_err_to_name(err));
        radio_teardown();
        return err;
    }

    uint8_t mac[MESH_MAC_LEN];
    char macs[MESH_MAC_STR_LEN];
    mesh_radio_get_mac(mac);
    mesh_format_mac(mac, macs);
    ESP_LOGI(TAG, "ESP-NOW up: mac=%s channel=%d lr=%d encryption=%d", macs, CONFIG_MESH_WIFI_CHANNEL,
             MESH_LR_ENABLED, mesh_radio_encryption_enabled() ? 1 : 0);
    return ESP_OK;
}

esp_err_t mesh_radio_add_peer(const uint8_t mac[MESH_MAC_LEN], bool encrypt)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, MESH_MAC_LEN);
    peer.channel = CONFIG_MESH_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = encrypt && mesh_radio_encryption_enabled() && !mesh_mac_is_broadcast(mac);
    if (peer.encrypt) {
        memcpy(peer.lmk, s_lmk, ESP_NOW_KEY_LEN);
    }
    esp_err_t err = esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        char macs[MESH_MAC_STR_LEN];
        mesh_format_mac(mac, macs);
        ESP_LOGW(TAG, "peer %s: %s", macs, esp_err_to_name(err));
    }
    return err;
}

esp_err_t mesh_radio_del_peer(const uint8_t mac[MESH_MAC_LEN])
{
    return esp_now_is_peer_exist(mac) ? esp_now_del_peer(mac) : ESP_OK;
}

bool mesh_radio_has_peer(const uint8_t mac[MESH_MAC_LEN])
{
    return esp_now_is_peer_exist(mac);
}

esp_err_t mesh_radio_send(const uint8_t mac[MESH_MAC_LEN], const uint8_t *data, size_t len)
{
    return esp_now_send(mac, data, len);
}
