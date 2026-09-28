/*
 * SPDX-License-Identifier: MIT
 * Node configuration: defaults, validation and remote updates.
 */
#include "mesh_protocol.h"
#include "node_config.h"
#include "unity.h"

static node_config_t cfg;

void setUp(void)
{
    node_config_defaults(&cfg, 42, 1000, 5000);
}
void tearDown(void) {}

static void test_defaults_are_valid(void)
{
    TEST_ASSERT_TRUE(node_config_is_valid(&cfg));
    TEST_ASSERT_EQUAL_UINT16(NODE_CONFIG_SCHEMA, cfg.schema);
}

static void test_invalid_blobs_are_rejected(void)
{
    node_config_t c = cfg;
    c.schema = NODE_CONFIG_SCHEMA + 1;
    TEST_ASSERT_FALSE(node_config_is_valid(&c));
    c = cfg;
    c.node_id = MESH_GATEWAY_NODE_ID;
    TEST_ASSERT_FALSE(node_config_is_valid(&c));
    c = cfg;
    c.node_id = MESH_BROADCAST_NODE_ID;
    TEST_ASSERT_FALSE(node_config_is_valid(&c));
    c = cfg;
    c.report_interval_ms = NODE_REPORT_MS_MIN - 1;
    TEST_ASSERT_FALSE(node_config_is_valid(&c));
    c = cfg;
    c.heartbeat_interval_ms = NODE_HB_MS_MAX + 1;
    TEST_ASSERT_FALSE(node_config_is_valid(&c));
}

static void test_apply_report_interval(void)
{
    TEST_ASSERT_EQUAL_INT(NODE_CFG_APPLIED, node_config_apply(&cfg, MESH_CFG_REPORT_INTERVAL_MS, 250));
    TEST_ASSERT_EQUAL_UINT32(250, cfg.report_interval_ms);
    TEST_ASSERT_EQUAL_INT(NODE_CFG_UNCHANGED, node_config_apply(&cfg, MESH_CFG_REPORT_INTERVAL_MS, 250));
}

static void test_apply_is_bounds_checked_and_atomic(void)
{
    TEST_ASSERT_EQUAL_INT(NODE_CFG_INVALID, node_config_apply(&cfg, MESH_CFG_REPORT_INTERVAL_MS, 5));
    TEST_ASSERT_EQUAL_INT(NODE_CFG_INVALID, node_config_apply(&cfg, MESH_CFG_HEARTBEAT_INTERVAL_MS, 999));
    TEST_ASSERT_EQUAL_INT(NODE_CFG_INVALID, node_config_apply(&cfg, 0x7F, 1));
    TEST_ASSERT_EQUAL_UINT32(1000, cfg.report_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(5000, cfg.heartbeat_interval_ms);
    TEST_ASSERT_TRUE(node_config_is_valid(&cfg));
}

static void test_apply_heartbeat_and_reboot(void)
{
    TEST_ASSERT_EQUAL_INT(NODE_CFG_APPLIED, node_config_apply(&cfg, MESH_CFG_HEARTBEAT_INTERVAL_MS, 10000));
    TEST_ASSERT_EQUAL_UINT32(10000, cfg.heartbeat_interval_ms);
    TEST_ASSERT_EQUAL_INT(NODE_CFG_REBOOT, node_config_apply(&cfg, MESH_CFG_REBOOT, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_invalid_blobs_are_rejected);
    RUN_TEST(test_apply_report_interval);
    RUN_TEST(test_apply_is_bounds_checked_and_atomic);
    RUN_TEST(test_apply_heartbeat_and_reboot);
    return UNITY_END();
}
