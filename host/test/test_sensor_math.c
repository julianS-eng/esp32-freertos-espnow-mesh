/*
 * SPDX-License-Identifier: MIT
 * Conversion maths of the real sensor drivers and the simulated backend.
 */
#include <math.h>
#include <string.h>

#include "sensor_math.h"
#include "sensor_sim.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_mq2_curve_passes_through_datasheet_points(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 200.0f, mq2_ppm_from_ratio(1.6f));
    TEST_ASSERT_FLOAT_WITHIN(20.0f, 10000.0f, mq2_ppm_from_ratio(0.26f));
    TEST_ASSERT_TRUE(mq2_ppm_from_ratio(0.5f) > mq2_ppm_from_ratio(1.0f)); /* lower Rs => more gas */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mq2_ppm_from_ratio(0.0f));
}

static void test_mq2_ratio_and_ppm_are_inverse(void)
{
    const float ppms[] = {50.0f, 200.0f, 1000.0f, 5000.0f, 10000.0f};
    for (size_t i = 0; i < sizeof(ppms) / sizeof(ppms[0]); i++) {
        TEST_ASSERT_FLOAT_WITHIN(ppms[i] * 1e-3f, ppms[i], mq2_ppm_from_ratio(mq2_ratio_from_ppm(ppms[i])));
    }
}

static void test_mq2_electrical_chain(void)
{
    const mq2_circuit_t c = {.vc_mv = 5000.0f, .rl_ohm = 1000.0f, .divider_ratio = 1.5f};
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1500.0f, mq2_sensor_mv(&c, 1000.0f));
    /* Vout = Vc/2 => Rs = RL */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1000.0f, mq2_rs_ohm(&c, 2500.0f));
    /* Clamps: no division by zero, no negative resistance */
    TEST_ASSERT_TRUE(isfinite(mq2_rs_ohm(&c, 0.0f)));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, mq2_rs_ohm(&c, 6000.0f));
    const mq2_circuit_t nodiv = {.vc_mv = 5000.0f, .rl_ohm = 1000.0f, .divider_ratio = 0.0f};
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1000.0f, mq2_sensor_mv(&nodiv, 1000.0f));
}

static void test_as5600_delta_unwraps(void)
{
    TEST_ASSERT_EQUAL_INT32(10, as5600_delta(100, 110));
    TEST_ASSERT_EQUAL_INT32(-10, as5600_delta(110, 100));
    TEST_ASSERT_EQUAL_INT32(6, as5600_delta(4090, 0));   /* forward through zero */
    TEST_ASSERT_EQUAL_INT32(-6, as5600_delta(0, 4090));  /* backward through zero */
    TEST_ASSERT_EQUAL_INT32(-2048, as5600_delta(0, 2048)); /* exactly half a turn is ambiguous */
    TEST_ASSERT_EQUAL_INT32(2047, as5600_delta(0, 2047));
    TEST_ASSERT_EQUAL_INT32(1, as5600_delta(0xF000 | 4095, 0)); /* upper bits ignored */
}

static void test_as5600_speed_and_angle(void)
{
    /* one revolution in 20 ms = 3000 rpm */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 3000.0f, as5600_rpm(4096, 20000));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, -3000.0f, as5600_rpm(-4096, 20000));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, as5600_rpm(100, 0));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, as5600_degrees(1024));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30000.0f, as5600_max_rpm(1000)); /* 1 kHz sampling */
}

static void test_as5600_sampled_rotation_reconstructs_speed(void)
{
    /* Emulate sampling a shaft at 1 kHz for 50 ms at 12000 rpm (200 rev/s). */
    const double rpm = 12000.0;
    uint16_t prev = 0;
    int64_t acc = 0;
    for (int k = 1; k <= 50; k++) {
        const double revs = rpm / 60.0 * (k * 1e-3);
        const uint16_t cur = (uint16_t)((uint32_t)llround(revs * 4096.0) % 4096u);
        acc += as5600_delta(prev, cur);
        prev = cur;
    }
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 12000.0f, as5600_rpm(acc, 50000));
}

static void test_mpu6050_conversions(void)
{
    TEST_ASSERT_EQUAL_FLOAT(8192.0f, mpu6050_accel_lsb_per_g(1));
    TEST_ASSERT_EQUAL_FLOAT(65.5f, mpu6050_gyro_lsb_per_dps(1));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 36.53f, mpu6050_temp_c(0));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 36.53f + 340.0f / 340.0f, mpu6050_temp_c(340));
    const uint8_t be[2] = {0xFF, 0x38}; /* -200 */
    TEST_ASSERT_EQUAL_INT16(-200, mpu6050_be16(be));
}

static void test_tilt_angles(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, tilt_pitch_deg(0.0f, 0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, tilt_roll_deg(0.0f, 0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, -90.0f, tilt_pitch_deg(1.0f, 0.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 45.0f, tilt_roll_deg(0.0f, 0.70710678f, 0.70710678f));
}

static void test_to_milli_rounds_and_saturates(void)
{
    TEST_ASSERT_EQUAL_INT32(1235, sensor_to_milli(1.2346f));
    TEST_ASSERT_EQUAL_INT32(-1235, sensor_to_milli(-1.2346f));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, sensor_to_milli(1e9f));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, sensor_to_milli(-1e9f));
}

static void read_sim(uint32_t seed, sensor_reading_t *out, int samples)
{
    sensor_sim_ctx_t ctx;
    sensor_sim_setup(&ctx, seed, SENSOR_SIM_ALL);
    sensor_driver_t d = sensor_sim_driver(&ctx);
    TEST_ASSERT_EQUAL_INT(SENSOR_OK, d.init(d.ctx));
    for (int i = 0; i < samples; i++) {
        sensor_reading_clear(out);
        TEST_ASSERT_EQUAL_INT(SENSOR_OK, d.read(d.ctx, (uint32_t)(i * 1000), out));
    }
}

static void test_sim_is_deterministic_per_seed(void)
{
    sensor_reading_t a, b, c;
    read_sim(42, &a, 30);
    read_sim(42, &b, 30);
    read_sim(43, &c, 30);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&a, &c, sizeof(a)));
}

static void test_sim_produces_all_channels_in_plausible_ranges(void)
{
    sensor_reading_t r;
    read_sim(7, &r, 120);
    TEST_ASSERT_EQUAL_UINT8(17, r.count);
    TEST_ASSERT_EQUAL_HEX8(MESH_BACKEND_SIM, r.backend_mask);
    for (uint8_t i = 0; i < r.count; i++) {
        const float v = (float)r.ch[i].value_milli / 1000.0f;
        switch (r.ch[i].quantity) {
        case MESH_Q_GAS_PPM:
            TEST_ASSERT_TRUE(v > 100.0f && v < 20000.0f);
            break;
        case MESH_Q_MOTOR_RPM:
            TEST_ASSERT_TRUE(v > 2000.0f && v < 4000.0f);
            break;
        case MESH_Q_ACCEL_Z:
            TEST_ASSERT_TRUE(v > 0.8f && v < 1.2f);
            break;
        case MESH_Q_MOTOR_ANGLE:
            TEST_ASSERT_TRUE(v >= 0.0f && v < 360.0f);
            break;
        default:
            break;
        }
    }
}

static void test_sim_warmup_flag_clears(void)
{
    sensor_reading_t r;
    read_sim(1, &r, 5);
    TEST_ASSERT_TRUE(r.status & MESH_STATUS_WARMING_UP);
    read_sim(1, &r, 30);
    TEST_ASSERT_FALSE(r.status & MESH_STATUS_WARMING_UP);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mq2_curve_passes_through_datasheet_points);
    RUN_TEST(test_mq2_ratio_and_ppm_are_inverse);
    RUN_TEST(test_mq2_electrical_chain);
    RUN_TEST(test_as5600_delta_unwraps);
    RUN_TEST(test_as5600_speed_and_angle);
    RUN_TEST(test_as5600_sampled_rotation_reconstructs_speed);
    RUN_TEST(test_mpu6050_conversions);
    RUN_TEST(test_tilt_angles);
    RUN_TEST(test_to_milli_rounds_and_saturates);
    RUN_TEST(test_sim_is_deterministic_per_seed);
    RUN_TEST(test_sim_produces_all_channels_in_plausible_ranges);
    RUN_TEST(test_sim_warmup_flag_clears);
    return UNITY_END();
}
