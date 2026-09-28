/*
 * SPDX-License-Identifier: MIT
 *
 * main.c - Sensor node entry point: NVS, identity, sensors, radio, tasks.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "node_app.h"
#include "sdkconfig.h"
#include "sensor_hal.h"

static const char *TAG = "main";

static node_ctx_t s_node; /* static: lives for the whole program, not on app_main's stack */

void app_main(void)
{
    ESP_ERROR_CHECK(node_storage_init());

    uint8_t mac[MESH_MAC_LEN];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    const uint16_t default_id = (CONFIG_NODE_ID != 0) ? (uint16_t)CONFIG_NODE_ID : mesh_node_id_from_mac(mac);
    if (node_storage_load(&s_node.cfg, default_id) != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable; running with defaults");
    }
    s_node.boot_count = node_storage_bump_boot_count();
    s_node.reset_reason = (uint8_t)esp_reset_reason();

    if (strlen(CONFIG_NODE_GATEWAY_MAC) > 0) {
        s_node.gateway_pinned = mesh_parse_mac(CONFIG_NODE_GATEWAY_MAC, s_node.gw_mac);
        if (!s_node.gateway_pinned) {
            ESP_LOGE(TAG, "NODE_GATEWAY_MAC '%s' is not a valid MAC; using discovery", CONFIG_NODE_GATEWAY_MAC);
        }
    }
#if CONFIG_MESH_ENCRYPTION
    if (!s_node.gateway_pinned) {
        ESP_LOGE(TAG, "encryption requires NODE_GATEWAY_MAC: the node cannot decrypt JOIN_ACK otherwise");
    }
#endif

    char macs[MESH_MAC_STR_LEN];
    mesh_format_mac(mac, macs);
    ESP_LOGI(TAG, "sensor node v%d.%d.%d id=0x%04x mac=%s boot=%" PRIu32 " reset=%u", NODE_FW_MAJOR, NODE_FW_MINOR,
             NODE_FW_PATCH, s_node.cfg.node_id, macs, s_node.boot_count, s_node.reset_reason);

    (void)sensor_hal_init(s_node.cfg.node_id); /* non-fatal: heartbeats still flow */
    ESP_ERROR_CHECK(node_tasks_start(&s_node));
    /* app_main returns; its task is deleted and the application tasks keep running. */
}
