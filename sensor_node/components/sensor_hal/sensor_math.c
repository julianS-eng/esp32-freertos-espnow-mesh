/*
 * SPDX-License-Identifier: MIT
 */
#include "sensor_math.h"

#include <math.h>

#define MQ2_CURVE_X0_PPM 200.0f
#define MQ2_CURVE_Y0 1.6f
#define MQ2_CURVE_X1_PPM 10000.0f
#define MQ2_CURVE_Y1 0.26f

static float mq2_slope(void)
{
    return (log10f(MQ2_CURVE_Y1) - log10f(MQ2_CURVE_Y0)) / (log10f(MQ2_CURVE_X1_PPM) - log10f(MQ2_CURVE_X0_PPM));
}

float mq2_sensor_mv(const mq2_circuit_t *c, float adc_mv)
{
    const float ratio = (c->divider_ratio >= 1.0f) ? c->divider_ratio : 1.0f;
    return adc_mv * ratio;
}

float mq2_rs_ohm(const mq2_circuit_t *c, float sensor_mv)
{
    if (sensor_mv < 1.0f) {
        sensor_mv = 1.0f; /* open circuit / no gas response: clamp instead of dividing by zero */
    }
    if (sensor_mv > c->vc_mv) {
        sensor_mv = c->vc_mv;
    }
    return c->rl_ohm * (c->vc_mv - sensor_mv) / sensor_mv;
}

float mq2_ppm_from_ratio(float rs_r0)
{
    if (rs_r0 <= 0.0f) {
        return 0.0f;
    }
    const float lg = log10f(MQ2_CURVE_X0_PPM) + (log10f(rs_r0) - log10f(MQ2_CURVE_Y0)) / mq2_slope();
    return powf(10.0f, lg);
}

float mq2_ratio_from_ppm(float ppm)
{
    if (ppm <= 0.0f) {
        return MQ2_CLEAN_AIR_RATIO;
    }
    const float lg = log10f(MQ2_CURVE_Y0) + mq2_slope() * (log10f(ppm) - log10f(MQ2_CURVE_X0_PPM));
    return powf(10.0f, lg);
}

int32_t as5600_delta(uint16_t prev, uint16_t cur)
{
    int32_t d = (int32_t)(cur & 0x0FFFu) - (int32_t)(prev & 0x0FFFu);
    if (d >= AS5600_COUNTS_PER_REV / 2) {
        d -= AS5600_COUNTS_PER_REV;
    } else if (d < -AS5600_COUNTS_PER_REV / 2) {
        d += AS5600_COUNTS_PER_REV;
    }
    return d;
}

float as5600_degrees(uint16_t raw)
{
    return (float)(raw & 0x0FFFu) * (360.0f / (float)AS5600_COUNTS_PER_REV);
}

float as5600_rpm(int64_t counts, int64_t dt_us)
{
    if (dt_us <= 0) {
        return 0.0f;
    }
    const double revs = (double)counts / (double)AS5600_COUNTS_PER_REV;
    return (float)(revs * 60.0e6 / (double)dt_us);
}

float as5600_max_rpm(uint32_t period_us)
{
    if (period_us == 0) {
        return 0.0f;
    }
    return (float)(0.5 * 60.0e6 / (double)period_us);
}

float mpu6050_accel_lsb_per_g(uint8_t afs_sel)
{
    static const float lsb[4] = {16384.0f, 8192.0f, 4096.0f, 2048.0f};
    return lsb[afs_sel & 3u];
}

float mpu6050_gyro_lsb_per_dps(uint8_t fs_sel)
{
    static const float lsb[4] = {131.0f, 65.5f, 32.8f, 16.4f};
    return lsb[fs_sel & 3u];
}

float mpu6050_temp_c(int16_t raw)
{
    return (float)raw / 340.0f + 36.53f;
}

int16_t mpu6050_be16(const uint8_t *p)
{
    return (int16_t)(uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

float tilt_pitch_deg(float ax, float ay, float az)
{
    return atan2f(-ax, sqrtf(ay * ay + az * az)) * (180.0f / (float)M_PI);
}

float tilt_roll_deg(float ax, float ay, float az)
{
    (void)ax;
    return atan2f(ay, az) * (180.0f / (float)M_PI);
}
