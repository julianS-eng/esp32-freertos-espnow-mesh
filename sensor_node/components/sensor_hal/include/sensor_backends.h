/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_backends.h - Constructors of the hardware drivers (ESP-IDF only).
 */
#pragma once

#include "sensor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

sensor_driver_t sensor_mq2_driver(void);
sensor_driver_t sensor_motor_as5600_driver(void);
sensor_driver_t sensor_mpu6050_driver(void);

#ifdef __cplusplus
}
#endif
