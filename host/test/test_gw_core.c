/*
 * SPDX-License-Identifier: MIT
 * Gateway per-frame decision procedure (the RX task's logic).
 */
#include <string.h>

#include "gw_core.h"
#include "unity.h"

static gw_core_t core;
static gw_rx_result_t res;
static uint8_t buf[MESH_FRAME_MAX];
static const uint8_t MAC_A[6] = {0x24, 0x0a, 0xc4, 0, 0, 1};
static const uint8_t MAC_B[6] = {0x24, 0x0a, 0xc4, 0, 0, 2};

void setUp(void)
{
    const gw_liveness_policy_t pol = {.suspect_misses = 2, .offline_misses = 4, .grace_ms = 1000};
    gw_core_init(&core, 4, &pol, 6);
}
void tearDown(void) {}

static size_t enc(mesh_frame_t *f)
{
    size_t len;
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_encode(f, buf, sizeof(buf), &len));
    return len;
}

static void rx(const uint8_t *mac, size_t len, uint64_t now, bool can_forward)
{
    gw_core_handle_frame(&core, mac, buf, len, -55, now, can_forward, &res);
}

static void decode_reply(mesh_frame_t *out)
{
    TEST_ASSERT_TRUE(res.reply);
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(res.reply_buf, res.reply_len, out));
    TEST_ASSERT_EQUAL_UINT16(MESH_GATEWAY_NODE_ID, out->hdr.node_id);
}

static void join(const uint8_t *mac, uint16_t id, uint16_t seq)
{
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_JOIN, id, seq, 10, 0);
    f.u.join.heartbeat_interval_ms = 5000;
    f.u.join.report_interval_ms = 1000;
    f.u.join.backend = MESH_BACKEND_SIM;
    rx(mac, enc(&f), 0, true);
}

static void send_data(const uint8_t *mac, uint16_t id, uint16_t seq, uint8_t attempt, uint64_t now, bool fwd)
{
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_DATA, id, seq, 100, MESH_FLAG_ACK_REQ);
    f.hdr.attempt = attempt;
    mesh_data_add(&f, MESH_Q_GAS_PPM, 180000);
    rx(mac, enc(&f), now, fwd);
}

static void test_join_is_accepted_and_answered(void)
{
    join(MAC_A, 5, 0);
    mesh_frame_t r;
    decode_reply(&r);
    TEST_ASSERT_EQUAL_UINT8(MESH_MSG_JOIN_ACK, r.hdr.type);
    TEST_ASSERT_EQUAL_UINT8(MESH_JOIN_ACCEPTED, r.u.join_ack.status);
    TEST_ASSERT_EQUAL_UINT8(6, r.u.join_ack.wifi_channel);
    TEST_ASSERT_EQUAL_UINT32(21000, r.u.join_ack.offline_timeout_ms);
    TEST_ASSERT_TRUE(res.add_peer);
    TEST_ASSERT_TRUE(res.persist);
    TEST_ASSERT_EQUAL_MEMORY(MAC_A, res.reply_mac, 6);
    TEST_ASSERT_EQUAL_size_t(1, res.n_out);
    TEST_ASSERT_EQUAL_INT(GW_OUT_JOIN, res.out[0].kind);
    TEST_ASSERT_TRUE(res.out[0].u.join.is_new);
    join(MAC_A, 5, 0); /* re-join after reboot: accepted, nothing new to persist */
    TEST_ASSERT_FALSE(res.persist);
}

static void test_join_conflict_is_rejected_with_temp_peer(void)
{
    join(MAC_A, 5, 0);
    join(MAC_B, 5, 0);
    mesh_frame_t r;
    decode_reply(&r);
    TEST_ASSERT_EQUAL_UINT8(MESH_JOIN_REJECTED_ID_CONFLICT, r.u.join_ack.status);
    TEST_ASSERT_TRUE(res.reply_needs_temp_peer);
    TEST_ASSERT_FALSE(res.add_peer);
    join(MAC_B, MESH_GATEWAY_NODE_ID, 0); /* reserved id */
    decode_reply(&r);
    TEST_ASSERT_EQUAL_UINT8(MESH_JOIN_REJECTED_ID_CONFLICT, r.u.join_ack.status);
}

static void test_data_is_acked_and_logged(void)
{
    join(MAC_A, 5, 0);
    send_data(MAC_A, 5, 1, 1, 50, true);
    mesh_frame_t r;
    decode_reply(&r);
    TEST_ASSERT_EQUAL_UINT8(MESH_MSG_ACK, r.hdr.type);
    TEST_ASSERT_EQUAL_UINT16(1, r.u.ack.acked_seq);
    TEST_ASSERT_EQUAL_UINT8(MESH_MSG_DATA, r.u.ack.acked_type);
    TEST_ASSERT_EQUAL_UINT8(1, r.u.ack.acked_attempt);
    TEST_ASSERT_EQUAL_UINT8(MESH_ACK_OK, r.u.ack.status);
    TEST_ASSERT_EQUAL_INT8(-55, r.u.ack.rssi);
    TEST_ASSERT_EQUAL_size_t(1, res.n_out);
    TEST_ASSERT_EQUAL_INT(GW_OUT_DATA, res.out[0].kind);
    TEST_ASSERT_EQUAL_INT32(180000, res.out[0].u.data.channels[0].value_milli);
    TEST_ASSERT_EQUAL_UINT32(0, res.out[0].gap);
}

static void test_retransmission_is_deduplicated_and_reacked(void)
{
    join(MAC_A, 5, 0);
    send_data(MAC_A, 5, 1, 1, 50, true);
    send_data(MAC_A, 5, 1, 2, 90, true); /* our ACK was lost; node retries */
    mesh_frame_t r;
    decode_reply(&r);
    TEST_ASSERT_EQUAL_UINT8(MESH_ACK_DUPLICATE, r.u.ack.status);
    TEST_ASSERT_EQUAL_UINT8(2, r.u.ack.acked_attempt);
    TEST_ASSERT_EQUAL_INT(GW_OUT_DUP, res.out[0].kind);
    TEST_ASSERT_EQUAL_UINT32(1, core.cnt.duplicates);
    TEST_ASSERT_EQUAL_UINT32(1, gw_registry_find_id(&core.reg, 5)->data_frames);
}

static void test_gap_is_reported(void)
{
    join(MAC_A, 5, 0);
    send_data(MAC_A, 5, 1, 1, 50, true);
    send_data(MAC_A, 5, 4, 1, 60, true);
    TEST_ASSERT_EQUAL_UINT32(2, res.out[0].gap);
}

static void test_busy_when_output_is_full_and_not_accounted(void)
{
    join(MAC_A, 5, 0);
    send_data(MAC_A, 5, 1, 1, 50, false);
    mesh_frame_t r;
    decode_reply(&r);
    TEST_ASSERT_EQUAL_UINT8(MESH_ACK_BUSY, r.u.ack.status);
    TEST_ASSERT_EQUAL_size_t(0, res.n_out);
    TEST_ASSERT_EQUAL_UINT32(1, core.cnt.busy_acks);
    send_data(MAC_A, 5, 1, 2, 80, true); /* the retry is accepted as new, not a duplicate */
    TEST_ASSERT_EQUAL_INT(GW_OUT_DATA, res.out[0].kind);
    TEST_ASSERT_EQUAL_UINT8(MESH_SEQ_NEW, res.out[0].seq_result);
}

static void test_unknown_node_is_asked_to_rejoin(void)
{
    send_data(MAC_B, 9, 1, 1, 50, true);
    mesh_frame_t r;
    decode_reply(&r);
    TEST_ASSERT_EQUAL_UINT8(MESH_ACK_REJECTED, r.u.ack.status);
    TEST_ASSERT_TRUE(res.reply_needs_temp_peer);
    TEST_ASSERT_EQUAL_INT(GW_OUT_RX_ERROR, res.out[0].kind);
    TEST_ASSERT_EQUAL_INT(GW_RXERR_UNKNOWN_NODE, res.out[0].u.rx_error.err);
    TEST_ASSERT_EQUAL_UINT32(1, core.cnt.unknown_node);
    TEST_ASSERT_EQUAL_UINT32(0, core.cnt.rx_errors);
}

static void test_corrupt_frame_is_counted_and_not_answered(void)
{
    join(MAC_A, 5, 0);
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_DATA, 5, 1, 0, MESH_FLAG_ACK_REQ);
    const size_t len = enc(&f);
    buf[3] ^= 0x40;
    rx(MAC_A, len, 5, true);
    TEST_ASSERT_FALSE(res.reply);
    TEST_ASSERT_EQUAL_INT(GW_OUT_RX_ERROR, res.out[0].kind);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_CRC, res.out[0].u.rx_error.err);
    TEST_ASSERT_EQUAL_UINT16(5, res.out[0].node_id); /* attributed by MAC */
    TEST_ASSERT_EQUAL_UINT32(1, gw_registry_find_id(&core.reg, 5)->rx_errors);
}

static void test_node_ack_is_routed_to_command_path(void)
{
    join(MAC_A, 5, 0);
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_ACK, 5, 1, 0, 0);
    f.u.ack.acked_seq = 77;
    f.u.ack.status = MESH_ACK_OK;
    rx(MAC_A, enc(&f), 5, true);
    TEST_ASSERT_TRUE(res.ack_for_gateway);
    TEST_ASSERT_EQUAL_UINT16(77, res.ack.acked_seq);
    TEST_ASSERT_EQUAL_UINT16(5, res.ack_from_node);
    TEST_ASSERT_FALSE(res.reply);
}

static void test_wrong_direction_frames_are_errors(void)
{
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_CONFIG, 5, 1, 0, 0);
    rx(MAC_A, enc(&f), 5, true);
    TEST_ASSERT_EQUAL_INT(GW_RXERR_BAD_DIRECTION, res.out[0].u.rx_error.err);
}

static void test_liveness_tick_and_recovery_record(void)
{
    join(MAC_A, 5, 0);
    gw_out_t out[4];
    TEST_ASSERT_EQUAL_size_t(1, gw_core_tick(&core, 30000, out, 4));
    TEST_ASSERT_EQUAL_INT(GW_OUT_NODE_STATE, out[0].kind);
    TEST_ASSERT_EQUAL_INT(GW_NODE_OFFLINE, out[0].u.node_state.to);
    TEST_ASSERT_EQUAL_MEMORY(MAC_A, out[0].mac, 6);
    send_data(MAC_A, 5, 1, 1, 31000, true);
    TEST_ASSERT_EQUAL_size_t(2, res.n_out);
    TEST_ASSERT_EQUAL_INT(GW_OUT_NODE_STATE, res.out[0].kind);
    TEST_ASSERT_EQUAL_INT(GW_NODE_ONLINE, res.out[0].u.node_state.to);
    TEST_ASSERT_EQUAL_INT(GW_OUT_DATA, res.out[1].kind);
}

static void test_config_builder_and_stats(void)
{
    join(MAC_A, 5, 0);
    size_t len;
    uint16_t seq;
    uint8_t mac[6];
    TEST_ASSERT_FALSE(gw_core_build_config(&core, 99, MESH_CFG_REBOOT, 0, 0, buf, &len, &seq, mac));
    TEST_ASSERT_TRUE(gw_core_build_config(&core, 5, MESH_CFG_REPORT_INTERVAL_MS, 2000, 0, buf, &len, &seq, mac));
    mesh_frame_t f;
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &f));
    TEST_ASSERT_EQUAL_UINT8(MESH_MSG_CONFIG, f.hdr.type);
    TEST_ASSERT_TRUE(f.hdr.flags & MESH_FLAG_ACK_REQ);
    TEST_ASSERT_EQUAL_UINT32(2000, f.u.config.value);
    TEST_ASSERT_EQUAL_MEMORY(MAC_A, mac, 6);

    gw_out_t links[4];
    TEST_ASSERT_EQUAL_size_t(1, gw_core_link_records(&core, 1, links, 4));
    TEST_ASSERT_EQUAL_INT(GW_OUT_LINK, links[0].kind);
    gw_stats_t st;
    memset(&st, 0, sizeof(st));
    gw_core_fill_stats(&core, &st);
    TEST_ASSERT_EQUAL_UINT8(1, st.nodes_total);
    TEST_ASSERT_EQUAL_UINT8(1, st.nodes_online);
    TEST_ASSERT_EQUAL_UINT32(1, st.rx_frames);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_join_is_accepted_and_answered);
    RUN_TEST(test_join_conflict_is_rejected_with_temp_peer);
    RUN_TEST(test_data_is_acked_and_logged);
    RUN_TEST(test_retransmission_is_deduplicated_and_reacked);
    RUN_TEST(test_gap_is_reported);
    RUN_TEST(test_busy_when_output_is_full_and_not_accounted);
    RUN_TEST(test_unknown_node_is_asked_to_rejoin);
    RUN_TEST(test_corrupt_frame_is_counted_and_not_answered);
    RUN_TEST(test_node_ack_is_routed_to_command_path);
    RUN_TEST(test_wrong_direction_frames_are_errors);
    RUN_TEST(test_liveness_tick_and_recovery_record);
    RUN_TEST(test_config_builder_and_stats);
    return UNITY_END();
}
