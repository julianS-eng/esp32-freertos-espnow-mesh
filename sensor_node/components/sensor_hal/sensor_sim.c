/*
 * SPDX-License-Identifier: MIT
 */
#include "sensor_sim.h"

#include <math.h>
#include <string.h>

#include "mesh_rand.h"
#include "sensor_math.h"

#define TWO_PI 6.28318530718f

/* Approximately standard-normal sample (Irwin-Hall with n = 4). */
static float gauss(uint32_t *rng)
{
    float s = 0.0f;
    for (int i = 0; i < 4; i++) {
        s += (float)mesh_rand_unit(rng);
    }
    return (s - 2.0f) * 1.7320508f;
}

void sensor_sim_setup(sensor_sim_ctx_t *ctx, uint32_t seed, uint8_t mask)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->mask = mask;
    ctx->rng = seed ? seed : 1u;
}

static sensor_err_t sim_init(void *vctx)
{
    (void)vctx;
    return SENSOR_OK;
}

static void sim_gas(sensor_sim_ctx_t *c, float t_s, float dt_s, sensor_reading_t *out)
{
    /* Background ~180 ppm with Gaussian noise; Poisson-arriving plumes that
     * decay exponentially (tau = 20 s), mimicking a leak being ventilated. */
    const float rate_per_s = 0.004f;
    if ((float)mesh_rand_unit(&c->rng) < rate_per_s * dt_s) {
        c->gas_plume_ppm += 500.0f + 2500.0f * (float)mesh_rand_unit(&c->rng);
    }
    c->gas_plume_ppm *= expf(-dt_s / 20.0f);
    float ppm = 180.0f + c->gas_plume_ppm + 4.0f * gauss(&c->rng);
    if (ppm < 1.0f) {
        ppm = 1.0f;
    }
    /* Back-compute the electrical chain so all three channels are consistent. */
    const mq2_circuit_t circ = {.vc_mv = 5000.0f, .rl_ohm = 1000.0f, .divider_ratio = 1.5f};
    const float r0 = 10000.0f;
    const float rs = mq2_ratio_from_ppm(ppm) * r0;
    const float v_sensor = circ.vc_mv * circ.rl_ohm / (circ.rl_ohm + rs);
    const float adc_mv = v_sensor / circ.divider_ratio;
    sensor_reading_add(out, MESH_Q_GAS_ADC_MV, sensor_to_milli(adc_mv));
    sensor_reading_add(out, MESH_Q_GAS_RS_R0, sensor_to_milli(rs / r0));
    sensor_reading_add(out, MESH_Q_GAS_PPM, sensor_to_milli(ppm));
    if ((uint32_t)(t_s * 1000.0f) < SENSOR_SIM_WARMUP_MS) {
        out->status |= MESH_STATUS_WARMING_UP;
    }
}

static void sim_motor(sensor_sim_ctx_t *c, float t_s, float dt_s, sensor_reading_t *out)
{
    /* Speed set-point sweeps slowly; the shaft follows with a first-order lag. */
    const float target = 3000.0f + 400.0f * sinf(TWO_PI * t_s / 120.0f);
    const float alpha = 1.0f - expf(-dt_s / 2.0f);
    c->motor_rpm += alpha * (target - c->motor_rpm);
    const float rpm = c->motor_rpm + 15.0f * gauss(&c->rng);
    c->motor_angle_deg = fmodf(c->motor_angle_deg + rpm / 60.0f * 360.0f * dt_s, 360.0f);
    if (c->motor_angle_deg < 0.0f) {
        c->motor_angle_deg += 360.0f;
    }
    const float duty = 20.0f + target / 60.0f;
    sensor_reading_add(out, MESH_Q_MOTOR_RPM, sensor_to_milli(rpm));
    sensor_reading_add(out, MESH_Q_MOTOR_ANGLE, sensor_to_milli(c->motor_angle_deg));
    sensor_reading_add(out, MESH_Q_MOTOR_DUTY, sensor_to_milli(duty));
    sensor_reading_add(out, MESH_Q_ENC_STATUS, 0x20 * 1000); /* MD: magnet detected */
    sensor_reading_add(out, MESH_Q_ENC_AGC, sensor_to_milli(128.0f + 2.0f * gauss(&c->rng)));
}

static void sim_imu(sensor_sim_ctx_t *c, float t_s, sensor_reading_t *out)
{
    /* Slow tilt drift plus motor-induced vibration proportional to speed. */
    const float pitch = 2.0f * sinf(TWO_PI * t_s / 300.0f);
    const float roll = 1.0f * cosf(TWO_PI * t_s / 240.0f);
    const float pr = pitch * (float)M_PI / 180.0f;
    const float rr = roll * (float)M_PI / 180.0f;
    const float vib = 0.03f * (c->motor_rpm / 3000.0f);
    const float ax = -sinf(pr) + vib * gauss(&c->rng);
    const float ay = cosf(pr) * sinf(rr) + vib * gauss(&c->rng);
    const float az = cosf(pr) * cosf(rr) + vib * gauss(&c->rng);
    const float temp = 38.0f - 7.0f * expf(-t_s / 600.0f);
    sensor_reading_add(out, MESH_Q_ACCEL_X, sensor_to_milli(ax));
    sensor_reading_add(out, MESH_Q_ACCEL_Y, sensor_to_milli(ay));
    sensor_reading_add(out, MESH_Q_ACCEL_Z, sensor_to_milli(az));
    sensor_reading_add(out, MESH_Q_GYRO_X, sensor_to_milli(0.3f * gauss(&c->rng)));
    sensor_reading_add(out, MESH_Q_GYRO_Y, sensor_to_milli(0.3f * gauss(&c->rng)));
    sensor_reading_add(out, MESH_Q_GYRO_Z, sensor_to_milli(0.3f * gauss(&c->rng)));
    sensor_reading_add(out, MESH_Q_IMU_TEMP, sensor_to_milli(temp + 0.05f * gauss(&c->rng)));
    sensor_reading_add(out, MESH_Q_PITCH, sensor_to_milli(tilt_pitch_deg(ax, ay, az)));
    sensor_reading_add(out, MESH_Q_ROLL, sensor_to_milli(tilt_roll_deg(ax, ay, az)));
}

static sensor_err_t sim_read(void *vctx, uint32_t now_ms, sensor_reading_t *out)
{
    sensor_sim_ctx_t *c = (sensor_sim_ctx_t *)vctx;
    if (!c->started) {
        c->started = true;
        c->start_ms = now_ms;
        c->last_ms = now_ms;
        c->motor_rpm = 0.0f;
    }
    const float dt_s = (float)(uint32_t)(now_ms - c->last_ms) / 1000.0f;
    const float t_s = (float)(uint32_t)(now_ms - c->start_ms) / 1000.0f;
    c->last_ms = now_ms;

    if (c->mask & SENSOR_SIM_GAS) {
        sim_gas(c, t_s, dt_s, out);
    }
    if (c->mask & (SENSOR_SIM_MOTOR | SENSOR_SIM_IMU)) {
        /* IMU vibration depends on motor speed, so advance the motor model even
         * when its channels are not reported. */
        sensor_reading_t scratch;
        sensor_reading_clear(&scratch);
        sim_motor(c, t_s, dt_s, (c->mask & SENSOR_SIM_MOTOR) ? out : &scratch);
    }
    if (c->mask & SENSOR_SIM_IMU) {
        sim_imu(c, t_s, out);
    }
    out->backend_mask |= MESH_BACKEND_SIM;
    return SENSOR_OK;
}

sensor_driver_t sensor_sim_driver(sensor_sim_ctx_t *ctx)
{
    const sensor_driver_t d = {
        .name = "sim",
        .backend = MESH_BACKEND_SIM,
        .init = sim_init,
        .read = sim_read,
        .ctx = ctx,
    };
    return d;
}
