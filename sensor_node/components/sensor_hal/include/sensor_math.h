/*
 * SPDX-License-Identifier: MIT
 *
 * sensor_math.h - Pure conversion routines for the real sensor backends.
 * Separated from the drivers so they are unit-tested on the host.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------- MQ-2 --------------------------------------- */

/** Clean-air Rs/R0 from the MQ-2 datasheet (Hanwei), sensitivity figure. */
#define MQ2_CLEAN_AIR_RATIO 9.83f

typedef struct {
    float vc_mv;          /**< Circuit (heater/load) supply, typically 5000 mV. */
    float rl_ohm;         /**< Load resistor on the module (FC-22 boards: 1 kOhm; some 4.7/10 kOhm). */
    float divider_ratio;  /**< V_sensor / V_adc of the external divider (>= 1; 1 = no divider). */
} mq2_circuit_t;

/** Sensor output voltage (at the module AO pin) from the ADC pin voltage. */
float mq2_sensor_mv(const mq2_circuit_t *c, float adc_mv);

/** Sensing resistance Rs = RL * (Vc - Vout) / Vout. Returns a large value for Vout ~ 0. */
float mq2_rs_ohm(const mq2_circuit_t *c, float sensor_mv);

/**
 * LPG-equivalent concentration from Rs/R0 using the datasheet log-log line
 * through (200 ppm, 1.6) and (10000 ppm, 0.26): log10(ppm) = log10(200) +
 * (log10(ratio) - log10(1.6)) / slope. Accuracy is that of a single-point
 * curve read off a datasheet plot - indicative only, not a calibrated meter.
 */
float mq2_ppm_from_ratio(float rs_r0);

/** Inverse of mq2_ppm_from_ratio (used by the simulator). */
float mq2_ratio_from_ppm(float ppm);

/* ---------------------------- AS5600 ------------------------------------- */

#define AS5600_COUNTS_PER_REV 4096

/** Shortest signed step between two 12-bit angles, in [-2048, 2047]. */
int32_t as5600_delta(uint16_t prev, uint16_t cur);

/** Angle in degrees [0, 360) from a 12-bit raw angle. */
float as5600_degrees(uint16_t raw);

/** Speed from an accumulated (unwrapped) count delta over @p dt_us. */
float as5600_rpm(int64_t counts, int64_t dt_us);

/** Max speed that can be unwrapped unambiguously at sample period @p period_us (< half a turn per sample). */
float as5600_max_rpm(uint32_t period_us);

/* ---------------------------- MPU-6050 ----------------------------------- */

/** LSB per g for AFS_SEL 0..3 (+-2/4/8/16 g). */
float mpu6050_accel_lsb_per_g(uint8_t afs_sel);
/** LSB per deg/s for FS_SEL 0..3 (+-250/500/1000/2000 dps). */
float mpu6050_gyro_lsb_per_dps(uint8_t fs_sel);
/** Die temperature in degC from TEMP_OUT (register map rev 4.2, sec. 4.18). */
float mpu6050_temp_c(int16_t raw);
/** Big-endian register pair to int16. */
int16_t mpu6050_be16(const uint8_t *p);

/** Tilt angles (degrees) from a gravity vector: pitch about Y, roll about X. */
float tilt_pitch_deg(float ax, float ay, float az);
float tilt_roll_deg(float ax, float ay, float az);

#ifdef __cplusplus
}
#endif
