/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_types.h - Backend-agnostic sensor abstraction (pure C, host-buildable).
 *
 * Every backend implements the same small vtable. A reading is a list of
 * (quantity, value x 1000) channels, which maps 1:1 onto the DATA frame, so
 * adding a sensor never requires a protocol change - only a new quantity id.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mesh_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t backend_mask; /**< MESH_BACKEND_* of every backend that contributed */
    uint8_t status;       /**< MESH_STATUS_* */
    uint8_t count;
    mesh_channel_t ch[MESH_MAX_CHANNELS];
} sensor_reading_t;

/** Return codes (kept independent of esp_err_t so the layer builds on the host). */
typedef enum {
    SENSOR_OK = 0,
    SENSOR_ERR_IO = -1,       /**< Bus / peripheral error. */
    SENSOR_ERR_NOT_FOUND = -2, /**< Device did not answer or wrong WHO_AM_I. */
    SENSOR_ERR_FULL = -3,     /**< Reading has no room for more channels. */
    SENSOR_ERR_STATE = -4,    /**< Not initialised. */
} sensor_err_t;

typedef struct sensor_driver {
    const char *name;
    uint8_t backend; /**< MESH_BACKEND_* bit */
    sensor_err_t (*init)(void *ctx);
    /** Append this backend's channels to @p out. @p now_ms is monotonic uptime. */
    sensor_err_t (*read)(void *ctx, uint32_t now_ms, sensor_reading_t *out);
    void *ctx;
} sensor_driver_t;

static inline void sensor_reading_clear(sensor_reading_t *r)
{
    r->backend_mask = 0;
    r->status = 0;
    r->count = 0;
}

static inline sensor_err_t sensor_reading_add(sensor_reading_t *r, mesh_quantity_t q, int32_t value_milli)
{
    if (r->count >= MESH_MAX_CHANNELS) {
        return SENSOR_ERR_FULL;
    }
    r->ch[r->count].quantity = (uint8_t)q;
    r->ch[r->count].value_milli = value_milli;
    r->count++;
    return SENSOR_OK;
}

/** Convert a float to fixed-point x1000 with rounding and saturation. */
static inline int32_t sensor_to_milli(float v)
{
    const float scaled = v * 1000.0f;
    if (scaled >= 2147483520.0f) {
        return INT32_MAX;
    }
    if (scaled <= -2147483520.0f) {
        return INT32_MIN;
    }
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

#ifdef __cplusplus
}
#endif
