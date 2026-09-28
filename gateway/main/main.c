/*
 * SPDX-License-Identifier: MIT
 *
 * main.c - Gateway entry point. Drives the lifecycle FSM through boot,
 * registry restore and radio bring-up (with bounded retries), then starts the
 * runtime tasks. Every transition is emitted as a gw_state JSON line.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_app.h"
#include "mesh_radio.h"
#include "sdkconfig.h"

static const char *TAG = "gw_main";

static void emit_boot(int restored)
{
    gw_out_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.kind = GW_OUT_BOOT;
    rec.ts_ms = gw_now_ms();
    mesh_radio_get_mac(rec.mac);
    rec.u.boot.fw_version = mesh_fw_version(GW_FW_MAJOR, GW_FW_MINOR, GW_FW_PATCH);
    rec.u.boot.channel = mesh_radio_channel();
    rec.u.boot.encryption = mesh_radio_encryption_enabled();
    rec.u.boot.capacity = CONFIG_GW_MAX_NODES;
    rec.u.boot.restored = (int8_t)restored;
    gw_emit(&rec);
}

void app_main(void)
{
    ESP_ERROR_CHECK(gw_tasks_init()); /* output task first: boot records must reach the host */
    gw_dispatch(GW_EV_BOOT_DONE);     /* BOOT -> LOAD_REGISTRY */

    int restored = 0;
    if (gw_storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: registry will not persist");
    } else {
        gw_core_lock(portMAX_DELAY);
        restored = gw_storage_load(&gw_core()->reg);
        gw_core_unlock();
    }
    emit_boot(restored);
    gw_dispatch(restored < 0 ? GW_EV_REGISTRY_CORRUPT : GW_EV_REGISTRY_LOADED); /* -> RADIO_INIT */

    for (int attempt = 1;; attempt++) {
        if (mesh_radio_init(gw_radio_recv_cb(), gw_radio_send_cb()) == ESP_OK) {
            gw_dispatch(GW_EV_RADIO_READY); /* -> RUNNING */
            break;
        }
        gw_dispatch(GW_EV_RADIO_FAILED); /* -> FAULT */
        if (attempt >= CONFIG_GW_RADIO_INIT_RETRIES) {
            ESP_LOGE(TAG, "radio failed %d times: restarting", attempt);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000u << (attempt - 1))); /* 1, 2, 4, 8 s */
        gw_dispatch(GW_EV_RETRY_TIMER); /* -> RADIO_INIT */
    }

    /* Re-register the peers of every node restored from NVS. */
    gw_core_lock(portMAX_DELAY);
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
        const gw_node_t *n = &gw_core()->reg.nodes[i];
        if (n->used) {
            (void)mesh_radio_add_peer(n->mac, true);
        }
    }
    gw_core_unlock();

    ESP_ERROR_CHECK(gw_tasks_start());
    ESP_LOGI(TAG, "gateway v%d.%d.%d running, %d node(s) restored", GW_FW_MAJOR, GW_FW_MINOR, GW_FW_PATCH,
             restored);
#if CONFIG_GW_QUIET_LOGS
    esp_log_level_set("*", ESP_LOG_WARN);
#endif
}
