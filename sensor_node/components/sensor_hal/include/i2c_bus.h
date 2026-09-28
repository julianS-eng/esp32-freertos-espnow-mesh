/*
 * SPDX-License-Identifier: MIT
 *
 * i2c_bus.h - Lazily created I2C master bus shared by the AS5600 and the
 * MPU-6050, plus a mutex for multi-transaction sequences.
 *
 * The IDF i2c_master driver already serialises individual transactions; the
 * extra mutex makes *sequences* atomic (e.g. the 1 kHz AS5600 sampling window
 * or the MPU-6050 reset + configuration sequence) so another backend cannot
 * interleave traffic in the middle.
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_bus_add_device(uint8_t addr7, i2c_master_dev_handle_t *out);
bool i2c_bus_lock(TickType_t timeout);
void i2c_bus_unlock(void);

/** Read @p len bytes starting at register @p reg. */
esp_err_t i2c_bus_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len);
esp_err_t i2c_bus_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif
