/*
 * SPDX-License-Identifier: MIT
 *
 * node_storage.c - NVS persistence of the node configuration and boot counter.
 * The configuration is a single versioned blob: a schema mismatch or an
 * out-of-range value falls back to Kconfig defaults instead of trusting it.
 */
#include <string.h>

#include "esp_log.h"
#include "node_app.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "node_nvs";
#define NS "node"
#define KEY_CFG "cfg"
#define KEY_BOOT "boot_cnt"

esp_err_t node_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Partition layout changed or full: erase and start clean. */
        ESP_LOGW(TAG, "NVS partition needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t node_storage_load(node_config_t *cfg, uint16_t default_id)
{
    node_config_defaults(cfg, default_id, CONFIG_NODE_REPORT_INTERVAL_MS, CONFIG_NODE_HEARTBEAT_INTERVAL_MS);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    node_config_t stored;
    size_t len = sizeof(stored);
    err = nvs_get_blob(h, KEY_CFG, &stored, &len);
    if (err == ESP_OK && len == sizeof(stored) && node_config_is_valid(&stored)) {
        /* A Kconfig NODE_ID != 0 always wins over the stored id. */
        if (CONFIG_NODE_ID != 0) {
            stored.node_id = (uint16_t)CONFIG_NODE_ID;
        }
        *cfg = stored;
        ESP_LOGI(TAG, "config loaded: id=%u report=%" PRIu32 "ms hb=%" PRIu32 "ms", cfg->node_id,
                 cfg->report_interval_ms, cfg->heartbeat_interval_ms);
    } else {
        ESP_LOGI(TAG, "no valid stored config (%s); writing defaults", esp_err_to_name(err));
        err = nvs_set_blob(h, KEY_CFG, cfg, sizeof(*cfg));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    nvs_close(h);
    return err;
}

esp_err_t node_storage_save(const node_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_CFG, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint32_t node_storage_bump_boot_count(void)
{
    nvs_handle_t h;
    uint32_t count = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return 0;
    }
    (void)nvs_get_u32(h, KEY_BOOT, &count);
    count++;
    if (nvs_set_u32(h, KEY_BOOT, count) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    return count;
}
