/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_hal.h - Aggregates the backends selected in menuconfig behind one
 * init/read interface used by the sensor task.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sensor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_HAL_MAX_DRIVERS 4

/**
 * Register and initialise the backends enabled in Kconfig. A backend that
 * fails to initialise is logged and skipped (the node still reports the
 * others and sets MESH_STATUS_SENSOR_FAULT) rather than aborting boot.
 * @param node_id used to decorrelate simulated nodes.
 */
esp_err_t sensor_hal_init(uint16_t node_id);

/** Read every active backend into @p out (cleared first). */
esp_err_t sensor_hal_read(uint32_t now_ms, sensor_reading_t *out);

/** MESH_BACKEND_* mask of the backends that initialised successfully. */
uint8_t sensor_hal_backend_mask(void);

#ifdef __cplusplus
}
#endif
