/*
 * SPDX-License-Identifier: MIT
 * JSON Lines emitter: exact output for representative records.
 */
#include <string.h>

#include "gw_fsm.h"
#include "gw_json.h"
#include "unity.h"

static char buf[GW_JSON_LINE_MAX];
static gw_out_t rec;

void setUp(void)
{
    memset(&rec, 0, sizeof(rec));
    const uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0x00, 0x00, 0x07};
    memcpy(rec.mac, mac, 6);
    rec.ts_ms = 12345;
    rec.node_id = 7;
}
void tearDown(void) {}

static void test_milli_formatting(void)
{
    char s[24];
    TEST_ASSERT_EQUAL_INT(1, gw_json_milli(0, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("0", s);
    gw_json_milli(1500, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("1.5", s);
    gw_json_milli(-1, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("-0.001", s);
    gw_json_milli(-210050, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("-210.05", s);
    gw_json_milli(INT32_MIN, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("-2147483.648", s);
    gw_json_milli(INT32_MAX, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("2147483.647", s);
    TEST_ASSERT_EQUAL_INT(-1, gw_json_milli(123456, s, 4));
}

static void test_data_line(void)
{
    rec.kind = GW_OUT_DATA;
    rec.seq = 42;
    rec.attempt = 2;
    rec.rssi = -61;
    rec.gap = 1;
    rec.seq_result = MESH_SEQ_NEW;
    rec.node_uptime_ms = 999;
    rec.u.data.backend = MESH_BACKEND_MQ2 | MESH_BACKEND_MPU6050;
    rec.u.data.status = MESH_STATUS_WARMING_UP;
    rec.u.data.last_rtt_us = 2100;
    rec.u.data.channel_count = 3;
    rec.u.data.channels[0] = (mesh_channel_t){MESH_Q_GAS_PPM, 210500};
    rec.u.data.channels[1] = (mesh_channel_t){MESH_Q_ACCEL_Z, -981};
    rec.u.data.channels[2] = (mesh_channel_t){99, 5};
    const int n = gw_json_format(&rec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"ts\":12345,\"type\":\"data\",\"node\":7,\"seq\":42,\"att\":2,\"rssi\":-61,\"gap\":1,"
                             "\"seq_state\":\"new\",\"up_ms\":999,\"rtt_us\":2100,\"status\":1,"
                             "\"backend\":[\"mq2\",\"mpu6050\"],"
                             "\"values\":{\"gas_ppm\":210.5,\"accel_z_g\":-0.981,\"q99\":0.005}}\n",
                             buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

static void test_node_state_and_gw_state_lines(void)
{
    rec.kind = GW_OUT_NODE_STATE;
    rec.u.node_state = (gw_transition_t){.node_id = 7, .from = GW_NODE_ONLINE, .to = GW_NODE_SUSPECT, .silent_ms = 11001};
    gw_json_format(&rec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":12345,\"type\":\"node_state\",\"node\":7,\"from\":\"online\",\"to\":\"suspect\",\"silent_ms\":11001}\n", buf);

    memset(&rec.u, 0, sizeof(rec.u));
    rec.kind = GW_OUT_GW_STATE;
    rec.u.gw_state.from = GW_ST_RUNNING;
    rec.u.gw_state.to = GW_ST_DEGRADED;
    rec.u.gw_state.event = GW_EV_QUEUE_PRESSURE;
    gw_json_format(&rec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":12345,\"type\":\"gw_state\",\"from\":\"running\",\"to\":\"degraded\",\"event\":\"queue_pressure\"}\n", buf);
}

static void test_rx_error_without_known_node(void)
{
    rec.kind = GW_OUT_RX_ERROR;
    rec.node_id = MESH_BROADCAST_NODE_ID;
    rec.u.rx_error.err = MESH_ERR_CRC;
    rec.u.rx_error.len = 30;
    gw_json_format(&rec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":12345,\"type\":\"rx_error\",\"mac\":\"24:0a:c4:00:00:07\",\"err\":\"bad_crc\",\"len\":30}\n", buf);
}

static void test_info_is_escaped(void)
{
    rec.kind = GW_OUT_INFO;
    strcpy(rec.u.info, "say \"hi\"\\\n");
    gw_json_format(&rec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"ts\":12345,\"type\":\"info\",\"msg\":\"say \\\"hi\\\"\\\\\\u000a\"}\n", buf);
}

static void test_every_kind_fits_line_budget(void)
{
    /* Worst case: a DATA frame with every channel at extreme values. */
    rec.kind = GW_OUT_DATA;
    rec.seq = 65535;
    rec.attempt = 255;
    rec.rssi = -128;
    rec.gap = UINT32_MAX;
    rec.node_uptime_ms = UINT32_MAX;
    rec.node_id = 65535;
    rec.u.data.backend = 0xFF;
    rec.u.data.status = 0xFF;
    rec.u.data.last_rtt_us = UINT32_MAX;
    rec.u.data.channel_count = MESH_MAX_CHANNELS;
    for (unsigned i = 0; i < MESH_MAX_CHANNELS; i++) {
        rec.u.data.channels[i] = (mesh_channel_t){(uint8_t)(1 + i % 17), INT32_MIN};
    }
    TEST_ASSERT_TRUE(gw_json_format(&rec, buf, sizeof(buf)) > 0);
    for (int k = GW_OUT_BOOT; k <= GW_OUT_INFO; k++) {
        memset(&rec.u, 0xFF, sizeof(rec.u));
        rec.kind = (gw_out_kind_t)k;
        if (k == GW_OUT_INFO) {
            memset(rec.u.info, '"', sizeof(rec.u.info) - 1);
            rec.u.info[sizeof(rec.u.info) - 1] = '\0';
        }
        if (k == GW_OUT_GW_STATS) {
            rec.u.stats.task_count = 0;
        }
        if (k == GW_OUT_HEARTBEAT) {
            rec.u.hb.task_count = MESH_HB_MAX_TASKS;
        }
        if (k == GW_OUT_BOOT) {
            rec.u.boot.encryption = true; /* bools must hold 0/1 */
        }
        if (k == GW_OUT_JOIN) {
            rec.u.join.is_new = true;
        }
        TEST_ASSERT_TRUE_MESSAGE(gw_json_format(&rec, buf, sizeof(buf)) > 0, "record exceeds GW_JSON_LINE_MAX");
        TEST_ASSERT_EQUAL_CHAR('\n', buf[strlen(buf) - 1]);
    }
}

static void test_too_small_buffer_reports_failure(void)
{
    rec.kind = GW_OUT_DUP;
    TEST_ASSERT_EQUAL_INT(-1, gw_json_format(&rec, buf, 10));
    TEST_ASSERT_EQUAL_INT(-1, gw_json_format(NULL, buf, sizeof(buf)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_milli_formatting);
    RUN_TEST(test_data_line);
    RUN_TEST(test_node_state_and_gw_state_lines);
    RUN_TEST(test_rx_error_without_known_node);
    RUN_TEST(test_info_is_escaped);
    RUN_TEST(test_every_kind_fits_line_budget);
    RUN_TEST(test_too_small_buffer_reports_failure);
    return UNITY_END();
}
