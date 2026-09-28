/*
 * SPDX-License-Identifier: MIT
 */
#include "sensor_hal.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "sensor_backends.h"
#include "sensor_sim.h"

static const char *TAG = "sensor_hal";

static sensor_driver_t s_drivers[SENSOR_HAL_MAX_DRIVERS];
static size_t s_count;
static uint8_t s_mask;

#if CONFIG_SENSOR_MODE_SIMULATED
static sensor_sim_ctx_t s_sim_ctx;
#endif

static void try_register(sensor_driver_t d)
{
    if (s_count >= SENSOR_HAL_MAX_DRIVERS) {
        ESP_LOGE(TAG, "too many drivers, '%s' ignored", d.name);
        return;
    }
    const sensor_err_t err = d.init(d.ctx);
    if (err != SENSOR_OK) {
        ESP_LOGE(TAG, "backend '%s' init failed (%d); continuing without it", d.name, (int)err);
        return;
    }
    s_drivers[s_count++] = d;
    s_mask |= d.backend;
    ESP_LOGI(TAG, "backend '%s' ready", d.name);
}

esp_err_t sensor_hal_init(uint16_t node_id)
{
    s_count = 0;
    s_mask = 0;
#if CONFIG_SENSOR_MODE_SIMULATED
    sensor_sim_setup(&s_sim_ctx, (uint32_t)CONFIG_SENSOR_SIM_SEED * 2654435761u ^ node_id, SENSOR_SIM_ALL);
    try_register(sensor_sim_driver(&s_sim_ctx));
#else
    (void)node_id;
#if CONFIG_SENSOR_MQ2_ENABLE
    try_register(sensor_mq2_driver());
#endif
#if CONFIG_SENSOR_MOTOR_ENABLE
    try_register(sensor_motor_as5600_driver());
#endif
#if CONFIG_SENSOR_MPU6050_ENABLE
    try_register(sensor_mpu6050_driver());
#endif
#endif
    if (s_count == 0) {
        ESP_LOGW(TAG, "no sensor backend available; node will only send heartbeats");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t sensor_hal_read(uint32_t now_ms, sensor_reading_t *out)
{
    sensor_reading_clear(out);
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < s_count; i++) {
        const sensor_err_t err = s_drivers[i].read(s_drivers[i].ctx, now_ms, out);
        if (err == SENSOR_OK) {
            out->backend_mask |= s_drivers[i].backend;
        } else {
            out->status |= MESH_STATUS_SENSOR_FAULT;
            ESP_LOGW(TAG, "backend '%s' read failed (%d)", s_drivers[i].name, (int)err);
            ret = ESP_FAIL;
        }
    }
    return ret;
}

uint8_t sensor_hal_backend_mask(void)
{
    return s_mask;
}
