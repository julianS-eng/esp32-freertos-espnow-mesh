/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_motor_as5600.c - Brushed R380 motor (6-24 V) driven open-loop by an
 * H-bridge (LEDC PWM + direction pin) with speed/angle feedback from an
 * AS5600 12-bit contactless magnetic encoder on the shaft.
 *
 * The motor must be powered from its own supply through the H-bridge; the
 * ESP32-S3 only provides logic-level PWM/DIR signals.
 */
#include "sdkconfig.h"

#if CONFIG_SENSOR_MOTOR_ENABLE

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "sensor_backends.h"
#include "sensor_math.h"

static const char *TAG = "motor";

#define AS5600_ADDR 0x36
#define AS5600_REG_STATUS 0x0B
#define AS5600_REG_RAW_ANGLE 0x0C
#define AS5600_REG_AGC 0x1A
#define AS5600_STATUS_MD 0x20 /* magnet detected */
#define AS5600_STATUS_ML 0x10 /* magnet too weak */
#define AS5600_STATUS_MH 0x08 /* magnet too strong */

#define PWM_TIMER LEDC_TIMER_0
#define PWM_CHANNEL LEDC_CHANNEL_0
#define PWM_RES LEDC_TIMER_10_BIT
#define PWM_MAX ((1u << 10) - 1u)

typedef struct {
    i2c_master_dev_handle_t dev;
    bool ready;
    uint32_t start_ms;
    bool started;
    float duty_pct;
} motor_ctx_t;

static motor_ctx_t s_ctx;

static void set_duty(float pct)
{
    if (pct < 0.0f) {
        pct = 0.0f;
    }
    if (pct > 100.0f) {
        pct = 100.0f;
    }
    const uint32_t duty = (uint32_t)(pct / 100.0f * (float)PWM_MAX + 0.5f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PWM_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PWM_CHANNEL);
}

static sensor_err_t motor_init(void *vctx)
{
    motor_ctx_t *c = (motor_ctx_t *)vctx;
    memset(c, 0, sizeof(*c));

    const ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = PWM_RES,
        .timer_num = PWM_TIMER,
        .freq_hz = CONFIG_SENSOR_MOTOR_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tcfg) != ESP_OK) {
        return SENSOR_ERR_IO;
    }
    const ledc_channel_config_t ccfg = {
        .gpio_num = CONFIG_SENSOR_MOTOR_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = PWM_CHANNEL,
        .timer_sel = PWM_TIMER,
        .duty = 0, /* motor off until the first read starts the soft-start ramp */
        .hpoint = 0,
    };
    if (ledc_channel_config(&ccfg) != ESP_OK) {
        return SENSOR_ERR_IO;
    }
#if CONFIG_SENSOR_MOTOR_DIR_GPIO >= 0
    gpio_reset_pin(CONFIG_SENSOR_MOTOR_DIR_GPIO);
    gpio_set_direction(CONFIG_SENSOR_MOTOR_DIR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_SENSOR_MOTOR_DIR_GPIO, 0);
#endif

    if (i2c_bus_add_device(AS5600_ADDR, &c->dev) != ESP_OK) {
        return SENSOR_ERR_NOT_FOUND;
    }
    uint8_t status = 0;
    if (i2c_bus_read_reg(c->dev, AS5600_REG_STATUS, &status, 1) != ESP_OK) {
        return SENSOR_ERR_IO;
    }
    if (!(status & AS5600_STATUS_MD)) {
        ESP_LOGW(TAG, "AS5600: no magnet detected (status 0x%02x)", status);
    } else if (status & (AS5600_STATUS_ML | AS5600_STATUS_MH)) {
        ESP_LOGW(TAG, "AS5600: magnet field out of range (status 0x%02x)", status);
    }
    ESP_LOGI(TAG, "R380 PWM on GPIO%d @ %d Hz, AS5600 ok, max unambiguous speed %.0f rpm",
             CONFIG_SENSOR_MOTOR_PWM_GPIO, CONFIG_SENSOR_MOTOR_PWM_FREQ_HZ,
             (double)as5600_max_rpm(1000000u / configTICK_RATE_HZ));
    c->ready = true;
    return SENSOR_OK;
}

static esp_err_t read_raw_angle(motor_ctx_t *c, uint16_t *raw)
{
    uint8_t b[2];
    esp_err_t err = i2c_bus_read_reg(c->dev, AS5600_REG_RAW_ANGLE, b, 2);
    *raw = (uint16_t)(((uint16_t)(b[0] & 0x0F) << 8) | b[1]);
    return err;
}

static sensor_err_t motor_read(void *vctx, uint32_t now_ms, sensor_reading_t *out)
{
    motor_ctx_t *c = (motor_ctx_t *)vctx;
    if (!c->ready) {
        return SENSOR_ERR_STATE;
    }
    if (!c->started) {
        c->started = true;
        c->start_ms = now_ms;
    }
    /* Soft-start: linear ramp to the configured duty. */
    const uint32_t elapsed = now_ms - c->start_ms;
    const float target = (float)CONFIG_SENSOR_MOTOR_DUTY_PCT;
    c->duty_pct = (elapsed >= CONFIG_SENSOR_MOTOR_RAMP_MS)
                      ? target
                      : target * (float)elapsed / (float)CONFIG_SENSOR_MOTOR_RAMP_MS;
    set_duty(c->duty_pct);

    if (!i2c_bus_lock(pdMS_TO_TICKS(100))) {
        return SENSOR_ERR_IO;
    }
    sensor_err_t ret = SENSOR_OK;
    uint16_t prev = 0, cur = 0;
    int64_t counts = 0;
    const int64_t t0 = esp_timer_get_time();
    int64_t t_last = t0;
    if (read_raw_angle(c, &prev) != ESP_OK) {
        ret = SENSOR_ERR_IO;
    }
    /* Sample once per tick; unwrap each step (< half a turn per sample). */
    TickType_t wake = xTaskGetTickCount();
    const TickType_t window = pdMS_TO_TICKS(CONFIG_SENSOR_MOTOR_RPM_WINDOW_MS);
    for (TickType_t n = 0; ret == SENSOR_OK && n < window; n++) {
        xTaskDelayUntil(&wake, 1);
        if (read_raw_angle(c, &cur) != ESP_OK) {
            ret = SENSOR_ERR_IO;
            break;
        }
        t_last = esp_timer_get_time();
        counts += as5600_delta(prev, cur);
        prev = cur;
    }
    uint8_t status = 0, agc = 0;
    if (ret == SENSOR_OK) {
        (void)i2c_bus_read_reg(c->dev, AS5600_REG_STATUS, &status, 1);
        (void)i2c_bus_read_reg(c->dev, AS5600_REG_AGC, &agc, 1);
    }
    i2c_bus_unlock();
    if (ret != SENSOR_OK) {
        return ret;
    }
    sensor_reading_add(out, MESH_Q_MOTOR_RPM, sensor_to_milli(as5600_rpm(counts, t_last - t0)));
    sensor_reading_add(out, MESH_Q_MOTOR_ANGLE, sensor_to_milli(as5600_degrees(cur)));
    sensor_reading_add(out, MESH_Q_MOTOR_DUTY, sensor_to_milli(c->duty_pct));
    sensor_reading_add(out, MESH_Q_ENC_STATUS, (int32_t)status * 1000);
    sensor_reading_add(out, MESH_Q_ENC_AGC, (int32_t)agc * 1000);
    if (!(status & AS5600_STATUS_MD)) {
        out->status |= MESH_STATUS_SENSOR_FAULT;
    }
    return SENSOR_OK;
}

sensor_driver_t sensor_motor_as5600_driver(void)
{
    const sensor_driver_t d = {.name = "motor_as5600",
                               .backend = MESH_BACKEND_MOTOR,
                               .init = motor_init,
                               .read = motor_read,
                               .ctx = &s_ctx};
    return d;
}

#endif /* CONFIG_SENSOR_MOTOR_ENABLE */
