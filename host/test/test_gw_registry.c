/*
 * SPDX-License-Identifier: MIT
 * Gateway node registry: join rules, liveness state machine, persistence.
 */
#include <string.h>

#include "gw_registry.h"
#include "unity.h"

static gw_registry_t reg;
static const gw_liveness_policy_t pol = {.suspect_misses = 2, .offline_misses = 4, .grace_ms = 1000};

static mesh_join_payload_t join_payload(uint32_t hb_ms)
{
    mesh_join_payload_t j;
    memset(&j, 0, sizeof(j));
    j.fw_version = 0x010000;
    j.boot_count = 1;
    j.report_interval_ms = 1000;
    j.heartbeat_interval_ms = hb_ms;
    j.backend = MESH_BACKEND_SIM;
    return j;
}

static void mac_of(uint8_t i, uint8_t mac[6])
{
    const uint8_t m[6] = {0x24, 0x0A, 0xC4, 0, 0, i};
    memcpy(mac, m, 6);
}

void setUp(void)
{
    gw_registry_init(&reg, 3, &pol);
}
void tearDown(void) {}

static void test_join_new_then_known(void)
{
    uint8_t mac[6];
    mac_of(1, mac);
    const mesh_join_payload_t j = join_payload(5000);
    gw_node_t *n = NULL;
    TEST_ASSERT_EQUAL_INT(GW_JOIN_OK_NEW, gw_registry_join(&reg, mac, 10, &j, 0, &n));
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(GW_NODE_ONLINE, n->state);
    TEST_ASSERT_EQUAL_INT(GW_JOIN_OK_KNOWN, gw_registry_join(&reg, mac, 10, &j, 100, &n));
    TEST_ASSERT_EQUAL_size_t(1, gw_registry_count(&reg));
    TEST_ASSERT_EQUAL_PTR(n, gw_registry_find_id(&reg, 10));
}

static void test_identity_change_requires_persist(void)
{
    uint8_t mac[6];
    mac_of(1, mac);
    mesh_join_payload_t j = join_payload(5000);
    gw_registry_join(&reg, mac, 10, &j, 0, NULL);
    j.heartbeat_interval_ms = 2000;
    TEST_ASSERT_EQUAL_INT(GW_JOIN_OK_NEW, gw_registry_join(&reg, mac, 10, &j, 0, NULL));
}

static void test_join_resets_sequence_tracking_only_after_reboot(void)
{
    uint8_t mac[6];
    mac_of(1, mac);
    mesh_join_payload_t j = join_payload(5000);
    gw_node_t *n;
    gw_registry_join(&reg, mac, 10, &j, 0, &n);
    uint32_t gap;
    bool tr;
    gw_transition_t t;
    gw_registry_on_frame(&reg, n, 500, -50, 1, &gap, &t, &tr);
    /* Re-join after link loss (same boot): tracker kept, the outage is a gap. */
    gw_registry_join(&reg, mac, 10, &j, 2, &n);
    TEST_ASSERT_TRUE(n->seq.initialized);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, gw_registry_on_frame(&reg, n, 505, -50, 3, &gap, &t, &tr));
    TEST_ASSERT_EQUAL_UINT32(4, gap);
    /* Reboot (boot_count changed): the node's counter restarted, so reset. */
    j.boot_count++;
    gw_registry_join(&reg, mac, 10, &j, 4, &n);
    TEST_ASSERT_FALSE(n->seq.initialized);
    TEST_ASSERT_EQUAL_INT(MESH_SEQ_NEW, gw_registry_on_frame(&reg, n, 0, -50, 5, &gap, &t, &tr));
    TEST_ASSERT_EQUAL_UINT32(0, n->seq.lost);
}

static void test_capacity_and_id_conflict(void)
{
    const mesh_join_payload_t j = join_payload(5000);
    uint8_t mac[6];
    for (uint8_t i = 1; i <= 3; i++) {
        mac_of(i, mac);
        TEST_ASSERT_EQUAL_INT(GW_JOIN_OK_NEW, gw_registry_join(&reg, mac, (uint16_t)(100 + i), &j, 0, NULL));
    }
    mac_of(9, mac);
    TEST_ASSERT_EQUAL_INT(GW_JOIN_FULL, gw_registry_join(&reg, mac, 200, &j, 0, NULL));
    gw_registry_init(&reg, 3, &pol);
    mac_of(1, mac);
    gw_registry_join(&reg, mac, 7, &j, 0, NULL);
    mac_of(2, mac);
    gw_node_t *n = (gw_node_t *)1;
    TEST_ASSERT_EQUAL_INT(GW_JOIN_ID_CONFLICT, gw_registry_join(&reg, mac, 7, &j, 0, &n));
    TEST_ASSERT_NULL(n);
}

static void test_liveness_thresholds_follow_heartbeat_interval(void)
{
    uint8_t mac[6];
    mac_of(1, mac);
    const mesh_join_payload_t j = join_payload(5000);
    gw_node_t *n;
    gw_registry_join(&reg, mac, 10, &j, 0, &n);
    TEST_ASSERT_EQUAL_UINT32(11000, gw_registry_suspect_ms(&reg, n));
    TEST_ASSERT_EQUAL_UINT32(21000, gw_registry_offline_ms(&reg, n));

    gw_transition_t tr[4];
    TEST_ASSERT_EQUAL_size_t(0, gw_registry_tick(&reg, 11000, tr, 4));
    TEST_ASSERT_EQUAL_size_t(1, gw_registry_tick(&reg, 11001, tr, 4));
    TEST_ASSERT_EQUAL_INT(GW_NODE_ONLINE, tr[0].from);
    TEST_ASSERT_EQUAL_INT(GW_NODE_SUSPECT, tr[0].to);
    TEST_ASSERT_EQUAL_size_t(0, gw_registry_tick(&reg, 15000, tr, 4)); /* no duplicate events */
    TEST_ASSERT_EQUAL_size_t(1, gw_registry_tick(&reg, 21001, tr, 4));
    TEST_ASSERT_EQUAL_INT(GW_NODE_OFFLINE, tr[0].to);
    TEST_ASSERT_EQUAL_UINT32(21001, tr[0].silent_ms);
    TEST_ASSERT_EQUAL_size_t(0, gw_registry_tick(&reg, 99000, tr, 4));

    /* Any frame brings it back, reporting the transition once. */
    uint32_t gap;
    bool changed;
    gw_transition_t t;
    gw_registry_on_frame(&reg, n, 1, -40, 30000, &gap, &t, &changed);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT(GW_NODE_OFFLINE, t.from);
    TEST_ASSERT_EQUAL_INT(GW_NODE_ONLINE, t.to);
    TEST_ASSERT_EQUAL_UINT32(30000, t.silent_ms);
    gw_registry_on_frame(&reg, n, 2, -40, 30001, &gap, &t, &changed);
    TEST_ASSERT_FALSE(changed);
}

static void test_skipping_suspect_goes_straight_offline(void)
{
    uint8_t mac[6];
    mac_of(1, mac);
    const mesh_join_payload_t j = join_payload(1000);
    gw_registry_join(&reg, mac, 10, &j, 0, NULL);
    gw_transition_t tr[2];
    TEST_ASSERT_EQUAL_size_t(1, gw_registry_tick(&reg, 60000, tr, 2));
    TEST_ASSERT_EQUAL_INT(GW_NODE_OFFLINE, tr[0].to);
}

static void test_policy_is_sanitized(void)
{
    const gw_liveness_policy_t bad = {.suspect_misses = 0, .offline_misses = 0, .grace_ms = 0};
    gw_registry_init(&reg, 0, &bad);
    TEST_ASSERT_EQUAL_UINT8(GW_REGISTRY_CAPACITY, reg.capacity);
    TEST_ASSERT_TRUE(reg.policy.offline_misses > reg.policy.suspect_misses);
    gw_registry_init(&reg, 5, NULL);
    TEST_ASSERT_EQUAL_UINT8(2, reg.policy.suspect_misses);
}

static void test_forget(void)
{
    uint8_t mac[6], out[6];
    mac_of(4, mac);
    const mesh_join_payload_t j = join_payload(5000);
    gw_registry_join(&reg, mac, 44, &j, 0, NULL);
    TEST_ASSERT_TRUE(gw_registry_forget(&reg, 44, out));
    TEST_ASSERT_EQUAL_MEMORY(mac, out, 6);
    TEST_ASSERT_FALSE(gw_registry_forget(&reg, 44, NULL));
    TEST_ASSERT_EQUAL_size_t(0, gw_registry_count(&reg));
}

static void test_persistence_round_trip(void)
{
    uint8_t mac[6];
    for (uint8_t i = 1; i <= 3; i++) {
        mac_of(i, mac);
        mesh_join_payload_t j = join_payload(1000u * i);
        j.boot_count = 10u + i;
        gw_registry_join(&reg, mac, (uint16_t)(0x100 + i), &j, 0, NULL);
    }
    uint8_t blob[512];
    size_t len = 0;
    TEST_ASSERT_TRUE(gw_registry_serialize(&reg, blob, sizeof(blob), &len));
    TEST_ASSERT_EQUAL_size_t(gw_registry_blob_size(&reg), len);
    TEST_ASSERT_FALSE(gw_registry_serialize(&reg, blob, len - 1, &len));
    TEST_ASSERT_TRUE(gw_registry_serialize(&reg, blob, sizeof(blob), &len));

    gw_registry_t restored;
    gw_registry_init(&restored, 3, &pol);
    TEST_ASSERT_EQUAL_INT(3, gw_registry_deserialize(&restored, blob, len));
    for (uint8_t i = 1; i <= 3; i++) {
        gw_node_t *n = gw_registry_find_id(&restored, (uint16_t)(0x100 + i));
        TEST_ASSERT_NOT_NULL(n);
        mac_of(i, mac);
        TEST_ASSERT_EQUAL_MEMORY(mac, n->mac, 6);
        TEST_ASSERT_EQUAL_UINT32(1000u * i, n->heartbeat_interval_ms);
        TEST_ASSERT_EQUAL_UINT32(10u + i, n->boot_count);
        TEST_ASSERT_EQUAL_INT(GW_NODE_OFFLINE, n->state); /* not heard since boot */
    }
}

static void test_persistence_rejects_corruption(void)
{
    uint8_t mac[6];
    mac_of(1, mac);
    const mesh_join_payload_t j = join_payload(5000);
    gw_registry_join(&reg, mac, 1, &j, 0, NULL);
    uint8_t blob[128];
    size_t len;
    gw_registry_serialize(&reg, blob, sizeof(blob), &len);
    gw_registry_t r2;
    gw_registry_init(&r2, 3, &pol);
    blob[12] ^= 0x01; /* flip a bit in the record */
    TEST_ASSERT_EQUAL_INT(-1, gw_registry_deserialize(&r2, blob, len));
    blob[12] ^= 0x01;
    blob[4] = 99; /* unknown version */
    TEST_ASSERT_EQUAL_INT(-1, gw_registry_deserialize(&r2, blob, len));
    blob[4] = GW_REGISTRY_BLOB_VERSION;
    TEST_ASSERT_EQUAL_INT(-1, gw_registry_deserialize(&r2, blob, len - 1));
    TEST_ASSERT_EQUAL_INT(-1, gw_registry_deserialize(&r2, NULL, 0));
    TEST_ASSERT_EQUAL_INT(1, gw_registry_deserialize(&r2, blob, len));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_join_new_then_known);
    RUN_TEST(test_identity_change_requires_persist);
    RUN_TEST(test_join_resets_sequence_tracking_only_after_reboot);
    RUN_TEST(test_capacity_and_id_conflict);
    RUN_TEST(test_liveness_thresholds_follow_heartbeat_interval);
    RUN_TEST(test_skipping_suspect_goes_straight_offline);
    RUN_TEST(test_policy_is_sanitized);
    RUN_TEST(test_forget);
    RUN_TEST(test_persistence_round_trip);
    RUN_TEST(test_persistence_rejects_corruption);
    return UNITY_END();
}
