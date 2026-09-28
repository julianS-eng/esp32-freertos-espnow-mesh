/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_vectors.c - Emits golden frames produced by the firmware codec as
 * JSON, consumed by the Python conformance tests (tests/test_protocol.py) so
 * the Python decoder is checked against the C implementation byte for byte.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mesh_crc.h"
#include "mesh_protocol.h"

static int s_first = 1;

static void emit(const char *name, const mesh_frame_t *f)
{
    uint8_t buf[MESH_FRAME_MAX];
    size_t len = 0;
    if (mesh_encode(f, buf, sizeof(buf), &len) != MESH_OK) {
        fprintf(stderr, "encode failed: %s\n", name);
        return;
    }
    printf("%s  {\"name\": \"%s\", \"type\": %u, \"node_id\": %u, \"seq\": %u, \"uptime_ms\": %" PRIu32
           ", \"attempt\": %u, \"flags\": %u, \"hex\": \"",
           s_first ? "" : ",\n", name, f->hdr.type, f->hdr.node_id, f->hdr.seq, f->hdr.uptime_ms, f->hdr.attempt,
           f->hdr.flags);
    for (size_t i = 0; i < len; i++) {
        printf("%02x", buf[i]);
    }
    printf("\"}");
    s_first = 0;
}

int main(void)
{
    printf("{\n\"crc_check\": {\"input\": \"123456789\", \"crc\": %u},\n\"vectors\": [\n",
           mesh_crc16((const uint8_t *)"123456789", 9));
    mesh_frame_t f;

    mesh_frame_init(&f, MESH_MSG_JOIN, 0x0102, 0, 1234, 0);
    f.u.join.fw_version = 0x010203;
    f.u.join.boot_count = 7;
    f.u.join.report_interval_ms = 1000;
    f.u.join.heartbeat_interval_ms = 5000;
    f.u.join.backend = MESH_BACKEND_MQ2 | MESH_BACKEND_MPU6050;
    f.u.join.reset_reason = 3;
    emit("join", &f);

    mesh_frame_init(&f, MESH_MSG_JOIN_ACK, MESH_GATEWAY_NODE_ID, 9, 99, 0);
    f.u.join_ack.status = MESH_JOIN_ACCEPTED;
    f.u.join_ack.wifi_channel = 6;
    f.u.join_ack.offline_timeout_ms = 21000;
    emit("join_ack", &f);

    mesh_frame_init(&f, MESH_MSG_DATA, 0x0102, 0xFFFF, 0xDEADBEEF, MESH_FLAG_ACK_REQ | MESH_FLAG_ENCRYPTED);
    f.hdr.attempt = 3;
    f.u.data.backend = MESH_BACKEND_SIM;
    f.u.data.status = MESH_STATUS_WARMING_UP;
    f.u.data.last_rtt_us = 2345;
    mesh_data_add(&f, MESH_Q_GAS_PPM, 210500);
    mesh_data_add(&f, MESH_Q_ACCEL_Z, -981);
    mesh_data_add(&f, MESH_Q_MOTOR_RPM, INT32_MAX);
    mesh_data_add(&f, MESH_Q_ROLL, INT32_MIN);
    emit("data", &f);

    mesh_frame_init(&f, MESH_MSG_DATA, 1, 1, 0, 0);
    emit("data_empty", &f);

    mesh_frame_init(&f, MESH_MSG_HEARTBEAT, 0x0102, 500, 60000, MESH_FLAG_ACK_REQ);
    f.u.hb.uptime_s = 60;
    f.u.hb.free_heap = 300000;
    f.u.hb.min_free_heap = 290000;
    f.u.hb.tx_ok = 58;
    f.u.hb.tx_retries = 4;
    f.u.hb.tx_failed = 1;
    f.u.hb.queue_drops = 2;
    f.u.hb.rtt_avg_us = 2800;
    f.u.hb.rtt_max_us = 9100;
    f.u.hb.last_ack_rssi = -67;
    f.u.hb.task_count = 4;
    for (unsigned i = 0; i < MESH_HB_MAX_TASKS; i++) {
        f.u.hb.stack_hwm[i] = (uint16_t)(1000 + 100 * i);
    }
    emit("heartbeat", &f);

    mesh_frame_init(&f, MESH_MSG_ACK, MESH_GATEWAY_NODE_ID, 10, 100, 0);
    f.u.ack.acked_seq = 0xFFFF;
    f.u.ack.acked_type = MESH_MSG_DATA;
    f.u.ack.acked_attempt = 3;
    f.u.ack.status = MESH_ACK_DUPLICATE;
    f.u.ack.rssi = -128;
    emit("ack", &f);

    mesh_frame_init(&f, MESH_MSG_CONFIG, MESH_GATEWAY_NODE_ID, 11, 200, MESH_FLAG_ACK_REQ);
    f.u.config.key = MESH_CFG_HEARTBEAT_INTERVAL_MS;
    f.u.config.value = 10000;
    emit("config", &f);

    printf("\n]\n}\n");
    return 0;
}
