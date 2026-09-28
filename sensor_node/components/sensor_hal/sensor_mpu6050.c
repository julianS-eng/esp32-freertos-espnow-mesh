/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_mpu6050.c - InvenSense MPU-6050 (GY-521 breakout) over I2C.
 * +-4 g accelerometer, +-500 dps gyroscope, 44 Hz DLPF, 100 Hz sample rate,
 * boot-time gyro bias estimation, burst read of the 14 data registers.
 */
#include "sdkconfig.h"

#if CONFIG_SENSOR_MPU6050_ENABLE

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "sensor_backends.h"
#include "sensor_math.h"

static const char *TAG = "mpu6050";

#define REG_SMPLRT_DIV 0x19
#define REG_CONFIG 0x1A
#define REG_GYRO_CONFIG 0x1B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1 0x6B
#define REG_WHO_AM_I 0x75

#define AFS_SEL 1 /* +-4 g */
#define FS_SEL 1  /* +-500 dps */

typedef struct {
    i2c_master_dev_handle_t dev;
    bool ready;
    float gyro_bias[3];
} mpu_ctx_t;

static mpu_ctx_t s_ctx;

typedef struct {
    float a[3]; /* g */
    float g[3]; /* dps, raw (bias not removed) */
    float temp_c;
} mpu_sample_t;

static esp_err_t read_sample(mpu_ctx_t *c, mpu_sample_t *s)
{
    uint8_t b[14];
    esp_err_t err = i2c_bus_read_reg(c->dev, REG_ACCEL_XOUT_H, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    const float alsb = mpu6050_accel_lsb_per_g(AFS_SEL);
    const float glsb = mpu6050_gyro_lsb_per_dps(FS_SEL);
    for (int i = 0; i < 3; i++) {
        s->a[i] = (float)mpu6050_be16(&b[2 * i]) / alsb;
        s->g[i] = (float)mpu6050_be16(&b[8 + 2 * i]) / glsb;
    }
    s->temp_c = mpu6050_temp_c(mpu6050_be16(&b[6]));
    return ESP_OK;
}

static sensor_err_t mpu_init(void *vctx)
{
    mpu_ctx_t *c = (mpu_ctx_t *)vctx;
    memset(c, 0, sizeof(*c));
    if (i2c_bus_add_device(CONFIG_SENSOR_MPU6050_ADDR, &c->dev) != ESP_OK) {
        return SENSOR_ERR_NOT_FOUND;
    }
    if (!i2c_bus_lock(pdMS_TO_TICKS(500))) {
        return SENSOR_ERR_IO;
    }
    sensor_err_t ret = SENSOR_OK;
    uint8_t who = 0;
    if (i2c_bus_read_reg(c->dev, REG_WHO_AM_I, &who, 1) != ESP_OK) {
        ret = SENSOR_ERR_IO;
    } else if ((who & 0x7E) != 0x68) {
        /* Genuine parts report 0x68; many GY-521 clones (MPU-6500 dies) report 0x70/0x72. */
        ESP_LOGW(TAG, "WHO_AM_I = 0x%02x (expected 0x68); continuing, may be a clone", who);
    }
    if (ret == SENSOR_OK) {
        esp_err_t e = i2c_bus_write_reg(c->dev, REG_PWR_MGMT_1, 0x80); /* device reset */
        vTaskDelay(pdMS_TO_TICKS(100));
        e |= i2c_bus_write_reg(c->dev, REG_PWR_MGMT_1, 0x01); /* wake, PLL with X gyro reference */
        e |= i2c_bus_write_reg(c->dev, REG_CONFIG, 0x03);     /* DLPF 44 Hz */
        e |= i2c_bus_write_reg(c->dev, REG_SMPLRT_DIV, 9);    /* 1 kHz / (1 + 9) = 100 Hz */
        e |= i2c_bus_write_reg(c->dev, REG_GYRO_CONFIG, FS_SEL << 3);
        e |= i2c_bus_write_reg(c->dev, REG_ACCEL_CONFIG, AFS_SEL << 3);
        if (e != ESP_OK) {
            ret = SENSOR_ERR_IO;
        }
    }
    if (ret == SENSOR_OK && CONFIG_SENSOR_MPU6050_GYRO_CAL_SAMPLES > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        float sum[3] = {0};
        int n = 0;
        for (int i = 0; i < CONFIG_SENSOR_MPU6050_GYRO_CAL_SAMPLES; i++) {
            mpu_sample_t s;
            if (read_sample(c, &s) == ESP_OK) {
                for (int k = 0; k < 3; k++) {
                    sum[k] += s.g[k];
                }
                n++;
            }
            vTaskDelay(pdMS_TO_TICKS(10)); /* one new sample every 10 ms at 100 Hz */
        }
        if (n > 0) {
            for (int k = 0; k < 3; k++) {
                c->gyro_bias[k] = sum[k] / (float)n;
            }
        }
        ESP_LOGI(TAG, "gyro bias (dps): %.3f %.3f %.3f from %d samples", (double)c->gyro_bias[0],
                 (double)c->gyro_bias[1], (double)c->gyro_bias[2], n);
    }
    i2c_bus_unlock();
    c->ready = (ret == SENSOR_OK);
    return ret;
}

static sensor_err_t mpu_read(void *vctx, uint32_t now_ms, sensor_reading_t *out)
{
    (void)now_ms;
    mpu_ctx_t *c = (mpu_ctx_t *)vctx;
    if (!c->ready) {
        return SENSOR_ERR_STATE;
    }
    mpu_sample_t s;
    if (!i2c_bus_lock(pdMS_TO_TICKS(100))) {
        return SENSOR_ERR_IO;
    }
    const esp_err_t err = read_sample(c, &s);
    i2c_bus_unlock();
    if (err != ESP_OK) {
        return SENSOR_ERR_IO;
    }
    sensor_reading_add(out, MESH_Q_ACCEL_X, sensor_to_milli(s.a[0]));
    sensor_reading_add(out, MESH_Q_ACCEL_Y, sensor_to_milli(s.a[1]));
    sensor_reading_add(out, MESH_Q_ACCEL_Z, sensor_to_milli(s.a[2]));
    sensor_reading_add(out, MESH_Q_GYRO_X, sensor_to_milli(s.g[0] - c->gyro_bias[0]));
    sensor_reading_add(out, MESH_Q_GYRO_Y, sensor_to_milli(s.g[1] - c->gyro_bias[1]));
    sensor_reading_add(out, MESH_Q_GYRO_Z, sensor_to_milli(s.g[2] - c->gyro_bias[2]));
    sensor_reading_add(out, MESH_Q_IMU_TEMP, sensor_to_milli(s.temp_c));
    sensor_reading_add(out, MESH_Q_PITCH, sensor_to_milli(tilt_pitch_deg(s.a[0], s.a[1], s.a[2])));
    sensor_reading_add(out, MESH_Q_ROLL, sensor_to_milli(tilt_roll_deg(s.a[0], s.a[1], s.a[2])));
    return SENSOR_OK;
}

sensor_driver_t sensor_mpu6050_driver(void)
{
    const sensor_driver_t d = {
        .name = "mpu6050", .backend = MESH_BACKEND_MPU6050, .init = mpu_init, .read = mpu_read, .ctx = &s_ctx};
    return d;
}

#endif /* CONFIG_SENSOR_MPU6050_ENABLE */
