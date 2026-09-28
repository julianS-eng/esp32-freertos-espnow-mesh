/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_sim.h - Deterministic simulated backend (pure C, host-buildable).
 *
 * Produces physically plausible signals for the three real sensors so that
 * the full pipeline (tasks, radio, gateway, dashboard) can be exercised
 * without hardware. Given the same seed and call times it produces the same
 * values on the ESP32-S3 and on the host simulator.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sensor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_SIM_GAS 0x01u
#define SENSOR_SIM_MOTOR 0x02u
#define SENSOR_SIM_IMU 0x04u
#define SENSOR_SIM_ALL (SENSOR_SIM_GAS | SENSOR_SIM_MOTOR | SENSOR_SIM_IMU)

typedef struct {
    uint8_t mask;
    uint32_t rng;
    bool started;
    uint32_t start_ms;
    uint32_t last_ms;
    float gas_plume_ppm;
    float motor_rpm;
    float motor_angle_deg;
} sensor_sim_ctx_t;

/** Prepare a context. @p seed makes the sequence reproducible (mix in the node id). */
void sensor_sim_setup(sensor_sim_ctx_t *ctx, uint32_t seed, uint8_t mask);

/** Driver vtable bound to @p ctx. */
sensor_driver_t sensor_sim_driver(sensor_sim_ctx_t *ctx);

/** Warm-up time emulating the MQ-2 heater, during which MESH_STATUS_WARMING_UP is set. */
#define SENSOR_SIM_WARMUP_MS 20000u

#ifdef __cplusplus
}
#endif
