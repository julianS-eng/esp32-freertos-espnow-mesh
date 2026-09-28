/*
 * SPDX-License-Identifier: MIT
 *
 * gw_storage.c - NVS persistence of the node registry (identities only).
 */
#include <stdlib.h>

#include "esp_log.h"
#include "gw_app.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gw_nvs";
#define NS "gateway"
#define KEY "registry"

esp_err_t gw_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

int gw_storage_load(gw_registry_t *reg)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return 0; /* namespace does not exist yet: first boot */
    }
    size_t len = 0;
    int restored = 0;
    if (nvs_get_blob(h, KEY, NULL, &len) == ESP_OK && len > 0) {
        uint8_t *buf = malloc(len);
        if (buf != NULL && nvs_get_blob(h, KEY, buf, &len) == ESP_OK) {
            restored = gw_registry_deserialize(reg, buf, len);
        }
        free(buf);
    }
    nvs_close(h);
    return restored;
}

esp_err_t gw_storage_save(const uint8_t *blob, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY, blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
