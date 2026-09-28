/*
 * SPDX-License-Identifier: MIT
 * CRC-16/CCITT-FALSE: catalogue check value, table vs. bitwise reference,
 * incremental computation and error-detection properties.
 */
#include <string.h>

#include "mesh_crc.h"
#include "mesh_rand.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_check_value_matches_catalogue(void)
{
    const char *msg = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1, mesh_crc16((const uint8_t *)msg, strlen(msg)));
}

static void test_empty_input_returns_init(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, mesh_crc16((const uint8_t *)"", 0));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, mesh_crc16(NULL, 10));
}

static void test_table_matches_bitwise_reference_on_random_buffers(void)
{
    uint32_t rng = 12345u; /* fixed seed: reproducible */
    uint8_t buf[250];
    for (int round = 0; round < 2000; round++) {
        const size_t len = mesh_rand_below(&rng, sizeof(buf) + 1);
        for (size_t i = 0; i < len; i++) {
            buf[i] = (uint8_t)mesh_rand_next(&rng);
        }
        TEST_ASSERT_EQUAL_HEX16(mesh_crc16_reference(MESH_CRC16_INIT, buf, len), mesh_crc16(buf, len));
    }
}

static void test_incremental_equals_one_shot(void)
{
    const uint8_t data[] = {0x4D, 0x01, 0x03, 0x01, 0x10, 0x00, 0x2A, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t crc = MESH_CRC16_INIT;
    crc = mesh_crc16_update(crc, data, 5);
    crc = mesh_crc16_update(crc, data + 5, sizeof(data) - 5);
    TEST_ASSERT_EQUAL_HEX16(mesh_crc16(data, sizeof(data)), crc);
}

static void test_detects_every_single_bit_flip(void)
{
    uint8_t buf[64];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 37u + 11u);
    }
    const uint16_t good = mesh_crc16(buf, sizeof(buf));
    for (size_t byte = 0; byte < sizeof(buf); byte++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_NOT_EQUAL(good, mesh_crc16(buf, sizeof(buf)));
            buf[byte] ^= (uint8_t)(1u << bit);
        }
    }
}

static void test_detects_all_double_bit_flips_in_short_frame(void)
{
    /* CRC-16/CCITT has Hamming distance 4 for messages far longer than 250 B,
     * so every 2-bit error in a frame must be detected. Exhaustive on 32 bytes. */
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(0xA5u ^ i);
    }
    const uint16_t good = mesh_crc16(buf, sizeof(buf));
    const size_t nbits = sizeof(buf) * 8;
    for (size_t a = 0; a < nbits; a++) {
        for (size_t b = a + 1; b < nbits; b++) {
            buf[a / 8] ^= (uint8_t)(1u << (a % 8));
            buf[b / 8] ^= (uint8_t)(1u << (b % 8));
            TEST_ASSERT_NOT_EQUAL(good, mesh_crc16(buf, sizeof(buf)));
            buf[a / 8] ^= (uint8_t)(1u << (a % 8));
            buf[b / 8] ^= (uint8_t)(1u << (b % 8));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value_matches_catalogue);
    RUN_TEST(test_empty_input_returns_init);
    RUN_TEST(test_table_matches_bitwise_reference_on_random_buffers);
    RUN_TEST(test_incremental_equals_one_shot);
    RUN_TEST(test_detects_every_single_bit_flip);
    RUN_TEST(test_detects_all_double_bit_flips_in_short_frame);
    return UNITY_END();
}
