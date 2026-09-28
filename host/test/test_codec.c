/*
 * SPDX-License-Identifier: MIT
 * Frame serialisation: round trips for every message type, exact byte layout,
 * agreement with the packed structs, and rejection of malformed input.
 */
#include <string.h>

#include "mesh_crc.h"
#include "mesh_protocol.h"
#include "mesh_rand.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t buf[MESH_FRAME_MAX];

static size_t encode_ok(const mesh_frame_t *f)
{
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_encode(f, buf, sizeof(buf), &len));
    return len;
}

static void make_data(mesh_frame_t *f, uint8_t nch)
{
    mesh_frame_init(f, MESH_MSG_DATA, 0x0102, 0xFFFE, 123456789u, MESH_FLAG_ACK_REQ);
    f->u.data.backend = MESH_BACKEND_MQ2 | MESH_BACKEND_MPU6050;
    f->u.data.status = MESH_STATUS_WARMING_UP;
    f->u.data.last_rtt_us = 4321;
    for (uint8_t i = 0; i < nch; i++) {
        TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_data_add(f, (mesh_quantity_t)(1 + i % 17), -1000 * (int32_t)i - 7));
    }
}

static void test_header_byte_layout_is_little_endian(void)
{
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_ACK, 0xBEEF, 0x1234, 0x0A0B0C0D, MESH_FLAG_ACK_REQ);
    f.hdr.attempt = 3;
    f.u.ack.acked_seq = 0x5678;
    f.u.ack.acked_type = MESH_MSG_DATA;
    f.u.ack.acked_attempt = 2;
    f.u.ack.status = MESH_ACK_DUPLICATE;
    f.u.ack.rssi = -42;
    const size_t len = encode_ok(&f);
    TEST_ASSERT_EQUAL_size_t(MESH_FRAME_OVERHEAD + 6, len);
    const uint8_t expected_hdr[] = {0x4D, 0x01, 0x05, 0x01, 0xEF, 0xBE, 0x34, 0x12,
                                    0x0D, 0x0C, 0x0B, 0x0A, 0x03, 0x06};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_hdr, buf, sizeof(expected_hdr));
    const uint8_t expected_payload[] = {0x78, 0x56, 0x03, 0x02, 0x01, (uint8_t)-42};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_payload, buf + MESH_HEADER_SIZE, sizeof(expected_payload));
    const uint16_t crc = mesh_crc16(buf, len - 2);
    TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, buf[len - 2]);
    TEST_ASSERT_EQUAL_HEX8(crc >> 8, buf[len - 1]);
}

static void test_encoding_matches_packed_struct_image_on_little_endian_host(void)
{
    /* The packed structs are the normative layout description; on a
     * little-endian machine their memory image must equal the codec output. */
    const uint16_t probe = 1;
    if (*(const uint8_t *)&probe != 1) {
        TEST_IGNORE_MESSAGE("big-endian host");
    }
    mesh_frame_t f;
    make_data(&f, 5);
    const size_t len = encode_ok(&f);
    mesh_header_t hdr = f.hdr;
    hdr.payload_len = (uint8_t)(len - MESH_FRAME_OVERHEAD);
    TEST_ASSERT_EQUAL_MEMORY(&hdr, buf, MESH_HEADER_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(&f.u.data, buf + MESH_HEADER_SIZE, len - MESH_FRAME_OVERHEAD);
}

static void test_round_trip_data_all_channel_counts(void)
{
    for (uint8_t n = 0; n <= MESH_MAX_CHANNELS; n++) {
        mesh_frame_t in, out;
        make_data(&in, n);
        const size_t len = encode_ok(&in);
        TEST_ASSERT_EQUAL_size_t(MESH_FRAME_OVERHEAD + MESH_DATA_FIXED_SIZE + 5u * n, len);
        TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &out));
        TEST_ASSERT_EQUAL_UINT8(MESH_MSG_DATA, out.hdr.type);
        TEST_ASSERT_EQUAL_UINT16(0x0102, out.hdr.node_id);
        TEST_ASSERT_EQUAL_UINT16(0xFFFE, out.hdr.seq);
        TEST_ASSERT_EQUAL_UINT32(123456789u, out.hdr.uptime_ms);
        TEST_ASSERT_EQUAL_UINT8(1, out.hdr.attempt);
        TEST_ASSERT_EQUAL_UINT8(n, out.u.data.channel_count);
        TEST_ASSERT_EQUAL_UINT32(4321, out.u.data.last_rtt_us);
        for (uint8_t i = 0; i < n; i++) {
            TEST_ASSERT_EQUAL_UINT8(in.u.data.channels[i].quantity, out.u.data.channels[i].quantity);
            TEST_ASSERT_EQUAL_INT32(in.u.data.channels[i].value_milli, out.u.data.channels[i].value_milli);
        }
    }
}

static void test_data_add_rejects_overflow(void)
{
    mesh_frame_t f;
    make_data(&f, MESH_MAX_CHANNELS);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_NO_SPACE, mesh_data_add(&f, MESH_Q_GAS_PPM, 1));
    mesh_frame_t hb;
    mesh_frame_init(&hb, MESH_MSG_HEARTBEAT, 1, 1, 0, 0);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_ARG, mesh_data_add(&hb, MESH_Q_GAS_PPM, 1));
}

static void test_round_trip_heartbeat(void)
{
    mesh_frame_t in, out;
    mesh_frame_init(&in, MESH_MSG_HEARTBEAT, 7, 99, 5000, 0);
    in.u.hb.uptime_s = 3600;
    in.u.hb.free_heap = 8000000;
    in.u.hb.min_free_heap = 7900000;
    in.u.hb.tx_ok = 1000;
    in.u.hb.tx_retries = 12;
    in.u.hb.tx_failed = 1;
    in.u.hb.queue_drops = 2;
    in.u.hb.rtt_avg_us = 2100;
    in.u.hb.rtt_max_us = 45000;
    in.u.hb.last_ack_rssi = -67;
    in.u.hb.task_count = 4;
    for (unsigned i = 0; i < MESH_HB_MAX_TASKS; i++) {
        in.u.hb.stack_hwm[i] = (uint16_t)(1000 + i);
    }
    const size_t len = encode_ok(&in);
    TEST_ASSERT_EQUAL_size_t(MESH_FRAME_OVERHEAD + sizeof(mesh_heartbeat_payload_t), len);
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_MEMORY(&in.u.hb, &out.u.hb, sizeof(in.u.hb));
}

static void test_round_trip_join_join_ack_config(void)
{
    mesh_frame_t in, out;
    mesh_frame_init(&in, MESH_MSG_JOIN, 3, 0, 10, 0);
    in.u.join.fw_version = 0x010203;
    in.u.join.boot_count = 77;
    in.u.join.report_interval_ms = 1000;
    in.u.join.heartbeat_interval_ms = 5000;
    in.u.join.backend = MESH_BACKEND_SIM;
    in.u.join.reset_reason = 1;
    size_t len = encode_ok(&in);
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_MEMORY(&in.u.join, &out.u.join, sizeof(in.u.join));

    mesh_frame_init(&in, MESH_MSG_JOIN_ACK, MESH_GATEWAY_NODE_ID, 5, 10, 0);
    in.u.join_ack.status = MESH_JOIN_ACCEPTED;
    in.u.join_ack.wifi_channel = 6;
    in.u.join_ack.offline_timeout_ms = 15000;
    len = encode_ok(&in);
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_MEMORY(&in.u.join_ack, &out.u.join_ack, sizeof(in.u.join_ack));

    mesh_frame_init(&in, MESH_MSG_CONFIG, MESH_GATEWAY_NODE_ID, 6, 10, MESH_FLAG_ACK_REQ);
    in.u.config.key = MESH_CFG_REPORT_INTERVAL_MS;
    in.u.config.value = 250;
    len = encode_ok(&in);
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_MEMORY(&in.u.config, &out.u.config, sizeof(in.u.config));
}

static void test_decode_rejects_truncated_and_padded_frames(void)
{
    mesh_frame_t f, out;
    make_data(&f, 3);
    const size_t len = encode_ok(&f);
    for (size_t l = 0; l < len; l++) {
        TEST_ASSERT_NOT_EQUAL(MESH_OK, mesh_decode(buf, l, &out));
    }
    TEST_ASSERT_EQUAL_INT(MESH_ERR_TOO_SHORT, mesh_decode(buf, MESH_FRAME_OVERHEAD - 1, &out));
    TEST_ASSERT_EQUAL_INT(MESH_ERR_LENGTH, mesh_decode(buf, len - 1, &out));
    TEST_ASSERT_EQUAL_INT(MESH_ERR_LENGTH, mesh_decode(buf, len + 1, &out));
}

static void test_decode_rejects_bad_magic_version_crc(void)
{
    mesh_frame_t f, out;
    make_data(&f, 2);
    const size_t len = encode_ok(&f);

    buf[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(MESH_ERR_MAGIC, mesh_decode(buf, len, &out));
    buf[0] ^= 0xFF;

    buf[1] = MESH_PROTO_VERSION + 1;
    TEST_ASSERT_EQUAL_INT(MESH_ERR_VERSION, mesh_decode(buf, len, &out));
    buf[1] = MESH_PROTO_VERSION;

    buf[MESH_HEADER_SIZE + 3] ^= 0x10; /* corrupt payload */
    TEST_ASSERT_EQUAL_INT(MESH_ERR_CRC, mesh_decode(buf, len, &out));
    buf[MESH_HEADER_SIZE + 3] ^= 0x10;

    buf[len - 1] ^= 0x01; /* corrupt CRC itself */
    TEST_ASSERT_EQUAL_INT(MESH_ERR_CRC, mesh_decode(buf, len, &out));
    buf[len - 1] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &out));
}

/* Re-seal a hand-modified frame with a valid CRC so deeper checks are reached. */
static void reseal(uint8_t *b, size_t len)
{
    const uint16_t crc = mesh_crc16(b, len - 2);
    b[len - 2] = (uint8_t)crc;
    b[len - 1] = (uint8_t)(crc >> 8);
}

static void test_decode_rejects_unknown_type_and_inconsistent_payload(void)
{
    mesh_frame_t f, out;
    make_data(&f, 2);
    size_t len = encode_ok(&f);

    buf[2] = 0x7F; /* unknown type */
    reseal(buf, len);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_TYPE, mesh_decode(buf, len, &out));
    buf[2] = MESH_MSG_DATA;

    buf[MESH_HEADER_SIZE + 6] = 3; /* channel_count disagrees with payload_len */
    reseal(buf, len);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_PAYLOAD, mesh_decode(buf, len, &out));

    buf[MESH_HEADER_SIZE + 6] = MESH_MAX_CHANNELS + 1;
    reseal(buf, len);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_PAYLOAD, mesh_decode(buf, len, &out));

    /* ACK frame whose payload_len claims 7 bytes (valid CRC, wrong size) */
    mesh_frame_init(&f, MESH_MSG_ACK, 1, 1, 1, 0);
    len = encode_ok(&f);
    uint8_t big[MESH_FRAME_MAX];
    memcpy(big, buf, len - 2);
    big[13] = 7;
    big[len - 2] = 0;
    reseal(big, len + 1);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_PAYLOAD, mesh_decode(big, len + 1, &out));
}

static void test_encode_argument_and_capacity_checks(void)
{
    mesh_frame_t f;
    size_t len;
    make_data(&f, 4);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_ARG, mesh_encode(NULL, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(MESH_ERR_ARG, mesh_encode(&f, NULL, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(MESH_ERR_NO_SPACE, mesh_encode(&f, buf, 10, &len));
    f.u.data.channel_count = MESH_MAX_CHANNELS + 1;
    TEST_ASSERT_EQUAL_INT(MESH_ERR_PAYLOAD, mesh_encode(&f, buf, sizeof(buf), &len));
    f.hdr.type = 0;
    TEST_ASSERT_EQUAL_INT(MESH_ERR_TYPE, mesh_encode(&f, buf, sizeof(buf), &len));
}

static void test_set_attempt_patches_header_and_crc(void)
{
    mesh_frame_t f, out;
    make_data(&f, 1);
    const size_t len = encode_ok(&f);
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_frame_set_attempt(buf, len, 4));
    TEST_ASSERT_EQUAL_INT(MESH_OK, mesh_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_UINT8(4, out.hdr.attempt);
    TEST_ASSERT_EQUAL_INT(MESH_ERR_ARG, mesh_frame_set_attempt(buf, 3, 2));
}

static void test_fuzz_random_bytes_are_rejected_safely(void)
{
    /* Random garbage must be rejected safely (ASan/UBSan watch memory safety). */
    uint32_t rng = 0xC0FFEEu;
    mesh_frame_t out;
    unsigned accepted = 0;
    for (int i = 0; i < 200000; i++) {
        const size_t len = mesh_rand_below(&rng, MESH_FRAME_MAX + 8);
        for (size_t j = 0; j < len && j < sizeof(buf); j++) {
            buf[j] = (uint8_t)mesh_rand_next(&rng);
        }
        if (mesh_decode(buf, len > sizeof(buf) ? sizeof(buf) : len, &out) == MESH_OK) {
            accepted++;
        }
    }
    TEST_ASSERT_EQUAL_UINT(0, accepted);
}

static void test_mutated_valid_frames_are_rejected(void)
{
    /* Flip 1..3 random bits of valid frames: CRC must catch all of them. */
    uint32_t rng = 777u;
    mesh_frame_t f, out;
    for (int i = 0; i < 20000; i++) {
        make_data(&f, (uint8_t)mesh_rand_below(&rng, MESH_MAX_CHANNELS + 1));
        f.hdr.seq = (uint16_t)mesh_rand_next(&rng);
        const size_t len = encode_ok(&f);
        const unsigned flips = 1 + mesh_rand_below(&rng, 3);
        uint32_t positions[3];
        for (unsigned k = 0; k < flips; k++) {
            uint32_t p;
            bool dup;
            do {
                p = mesh_rand_below(&rng, (uint32_t)len * 8u);
                dup = false;
                for (unsigned m = 0; m < k; m++) {
                    dup |= positions[m] == p;
                }
            } while (dup);
            positions[k] = p;
            buf[p / 8] ^= (uint8_t)(1u << (p % 8));
        }
        TEST_ASSERT_NOT_EQUAL(MESH_OK, mesh_decode(buf, len, &out));
    }
}

static void test_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("data", mesh_msg_type_str(MESH_MSG_DATA));
    TEST_ASSERT_EQUAL_STRING("unknown", mesh_msg_type_str(0));
    TEST_ASSERT_EQUAL_STRING("bad_crc", mesh_err_str(MESH_ERR_CRC));
    TEST_ASSERT_EQUAL_STRING("gas_ppm", mesh_quantity_key(MESH_Q_GAS_PPM));
    TEST_ASSERT_EQUAL_STRING("roll_deg", mesh_quantity_key(MESH_Q_ROLL));
    TEST_ASSERT_NULL(mesh_quantity_key(0));
    TEST_ASSERT_NULL(mesh_quantity_key(MESH_Q_COUNT_));
    for (uint8_t q = 1; q < MESH_Q_COUNT_; q++) {
        TEST_ASSERT_NOT_NULL(mesh_quantity_key(q));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_byte_layout_is_little_endian);
    RUN_TEST(test_encoding_matches_packed_struct_image_on_little_endian_host);
    RUN_TEST(test_round_trip_data_all_channel_counts);
    RUN_TEST(test_data_add_rejects_overflow);
    RUN_TEST(test_round_trip_heartbeat);
    RUN_TEST(test_round_trip_join_join_ack_config);
    RUN_TEST(test_decode_rejects_truncated_and_padded_frames);
    RUN_TEST(test_decode_rejects_bad_magic_version_crc);
    RUN_TEST(test_decode_rejects_unknown_type_and_inconsistent_payload);
    RUN_TEST(test_encode_argument_and_capacity_checks);
    RUN_TEST(test_set_attempt_patches_header_and_crc);
    RUN_TEST(test_fuzz_random_bytes_are_rejected_safely);
    RUN_TEST(test_mutated_valid_frames_are_rejected);
    RUN_TEST(test_strings);
    return UNITY_END();
}
