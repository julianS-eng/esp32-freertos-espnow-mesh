/*
 * SPDX-License-Identifier: MIT
 * MAC / key helpers and node-id derivation.
 */
#include <string.h>

#include "mesh_protocol.h"
#include "mesh_util.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse_hex_key(void)
{
    uint8_t key[MESH_KEY_LEN];
    TEST_ASSERT_TRUE(mesh_parse_hex("a1b2c3d4e5f60718293a4b5c6d7e8f90", key, sizeof(key)));
    TEST_ASSERT_EQUAL_HEX8(0xA1, key[0]);
    TEST_ASSERT_EQUAL_HEX8(0x90, key[15]);
    TEST_ASSERT_TRUE(mesh_parse_hex("A1B2C3D4E5F60718293A4B5C6D7E8F90", key, sizeof(key)));
    TEST_ASSERT_FALSE(mesh_parse_hex("a1b2", key, sizeof(key)));                              /* too short */
    TEST_ASSERT_FALSE(mesh_parse_hex("a1b2c3d4e5f60718293a4b5c6d7e8f9g", key, sizeof(key))); /* bad char */
    TEST_ASSERT_FALSE(mesh_parse_hex(NULL, key, sizeof(key)));
}

static void test_parse_and_format_mac(void)
{
    uint8_t mac[MESH_MAC_LEN];
    char s[MESH_MAC_STR_LEN];
    TEST_ASSERT_TRUE(mesh_parse_mac("24:0A:c4:00:12:ff", mac));
    mesh_format_mac(mac, s);
    TEST_ASSERT_EQUAL_STRING("24:0a:c4:00:12:ff", s);
    TEST_ASSERT_TRUE(mesh_parse_mac("24-0a-c4-00-12-ff", mac));
    TEST_ASSERT_FALSE(mesh_parse_mac("24:0a:c4:00:12", mac));
    TEST_ASSERT_FALSE(mesh_parse_mac("24:0a:c4:00:12:fz", mac));
    TEST_ASSERT_FALSE(mesh_parse_mac("240ac40012ff00000", mac));
}

static void test_broadcast_detection(void)
{
    const uint8_t b[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t u[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    TEST_ASSERT_TRUE(mesh_mac_is_broadcast(b));
    TEST_ASSERT_FALSE(mesh_mac_is_broadcast(u));
}

static void test_node_id_from_mac_is_stable_and_never_reserved(void)
{
    const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x00, 0x12, 0xFF};
    const uint16_t a = mesh_node_id_from_mac(mac);
    TEST_ASSERT_EQUAL_UINT16(a, mesh_node_id_from_mac(mac));
    uint8_t m[6] = {0};
    for (uint32_t i = 0; i < 70000; i++) {
        m[4] = (uint8_t)(i >> 8);
        m[5] = (uint8_t)i;
        m[3] = (uint8_t)(i >> 16);
        const uint16_t id = mesh_node_id_from_mac(m);
        TEST_ASSERT_NOT_EQUAL(MESH_GATEWAY_NODE_ID, id);
        TEST_ASSERT_NOT_EQUAL(MESH_BROADCAST_NODE_ID, id);
    }
}

static void test_fw_version_packing(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x010203, mesh_fw_version(1, 2, 3));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_hex_key);
    RUN_TEST(test_parse_and_format_mac);
    RUN_TEST(test_broadcast_detection);
    RUN_TEST(test_node_id_from_mac_is_stable_and_never_reserved);
    RUN_TEST(test_fw_version_packing);
    return UNITY_END();
}
