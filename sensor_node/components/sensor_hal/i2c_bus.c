/*
 * SPDX-License-Identifier: MIT
 */
#include "i2c_bus.h"

#include "esp_log.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_SENSOR_MOTOR_ENABLE || CONFIG_SENSOR_MPU6050_ENABLE

static const char *TAG = "i2c_bus";
#define I2C_XFER_TIMEOUT_MS 20

static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_buf;

static esp_err_t ensure_bus(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1, /* auto-select */
        .sda_io_num = CONFIG_SENSOR_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_SENSOR_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* GY-521 / AS5600 boards also carry external pull-ups */
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t i2c_bus_add_device(uint8_t addr7, i2c_master_dev_handle_t *out)
{
    esp_err_t err = ensure_bus();
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_probe(s_bus, addr7, I2C_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no device at 0x%02x (%s)", addr7, esp_err_to_name(err));
        return err;
    }
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz = CONFIG_SENSOR_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev, out);
}

bool i2c_bus_lock(TickType_t timeout)
{
    return s_mutex != NULL && xSemaphoreTake(s_mutex, timeout) == pdTRUE;
}

void i2c_bus_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t i2c_bus_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, I2C_XFER_TIMEOUT_MS);
}

esp_err_t i2c_bus_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
}

#endif /* CONFIG_SENSOR_MOTOR_ENABLE || CONFIG_SENSOR_MPU6050_ENABLE */
