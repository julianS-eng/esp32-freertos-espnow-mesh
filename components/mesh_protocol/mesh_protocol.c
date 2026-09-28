/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_protocol.c - Frame codec. Serialises field by field in little-endian
 * order so the result is identical on the ESP32-S3 (Xtensa LX7, little-endian)
 * and on any host used for tests or simulation.
 */
#include "mesh_protocol.h"

#include <string.h>

#include "mesh_crc.h"

/* ------------------------------------------------------------------------- */
/* Little-endian cursor helpers                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t left;
    bool overflow;
} wcur_t;

typedef struct {
    const uint8_t *p;
    size_t left;
    bool underflow;
} rcur_t;

static void w_u8(wcur_t *c, uint8_t v)
{
    if (c->left < 1) {
        c->overflow = true;
        return;
    }
    *c->p++ = v;
    c->left--;
}

static void w_u16(wcur_t *c, uint16_t v)
{
    w_u8(c, (uint8_t)(v & 0xFFu));
    w_u8(c, (uint8_t)(v >> 8));
}

static void w_u32(wcur_t *c, uint32_t v)
{
    w_u16(c, (uint16_t)(v & 0xFFFFu));
    w_u16(c, (uint16_t)(v >> 16));
}

static uint8_t r_u8(rcur_t *c)
{
    if (c->left < 1) {
        c->underflow = true;
        return 0;
    }
    c->left--;
    return *c->p++;
}

static uint16_t r_u16(rcur_t *c)
{
    uint16_t lo = r_u8(c);
    uint16_t hi = r_u8(c);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static uint32_t r_u32(rcur_t *c)
{
    uint32_t lo = r_u16(c);
    uint32_t hi = r_u16(c);
    return lo | (hi << 16);
}

/* ------------------------------------------------------------------------- */
/* Payload sizes                                                             */
/* ------------------------------------------------------------------------- */

mesh_err_t mesh_payload_size(const mesh_frame_t *f, size_t *out_size)
{
    if (f == NULL || out_size == NULL) {
        return MESH_ERR_ARG;
    }
    switch ((mesh_msg_type_t)f->hdr.type) {
    case MESH_MSG_JOIN:
        *out_size = sizeof(mesh_join_payload_t);
        return MESH_OK;
    case MESH_MSG_JOIN_ACK:
        *out_size = sizeof(mesh_join_ack_payload_t);
        return MESH_OK;
    case MESH_MSG_DATA:
        if (f->u.data.channel_count > MESH_MAX_CHANNELS) {
            return MESH_ERR_PAYLOAD;
        }
        *out_size = MESH_DATA_FIXED_SIZE + (size_t)f->u.data.channel_count * sizeof(mesh_channel_t);
        return MESH_OK;
    case MESH_MSG_HEARTBEAT:
        *out_size = sizeof(mesh_heartbeat_payload_t);
        return MESH_OK;
    case MESH_MSG_ACK:
        *out_size = sizeof(mesh_ack_payload_t);
        return MESH_OK;
    case MESH_MSG_CONFIG:
        *out_size = sizeof(mesh_config_payload_t);
        return MESH_OK;
    default:
        return MESH_ERR_TYPE;
    }
}

/* ------------------------------------------------------------------------- */
/* Encode                                                                    */
/* ------------------------------------------------------------------------- */

void mesh_frame_init(mesh_frame_t *f, mesh_msg_type_t type, uint16_t node_id, uint16_t seq,
                     uint32_t uptime_ms, uint8_t flags)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->hdr.magic = MESH_PROTO_MAGIC;
    f->hdr.version = MESH_PROTO_VERSION;
    f->hdr.type = (uint8_t)type;
    f->hdr.flags = flags;
    f->hdr.node_id = node_id;
    f->hdr.seq = seq;
    f->hdr.uptime_ms = uptime_ms;
    f->hdr.attempt = 1;
}

static void encode_payload(wcur_t *c, const mesh_frame_t *f)
{
    switch ((mesh_msg_type_t)f->hdr.type) {
    case MESH_MSG_JOIN: {
        const mesh_join_payload_t *j = &f->u.join;
        w_u32(c, j->fw_version);
        w_u32(c, j->boot_count);
        w_u32(c, j->report_interval_ms);
        w_u32(c, j->heartbeat_interval_ms);
        w_u8(c, j->backend);
        w_u8(c, j->reset_reason);
        break;
    }
    case MESH_MSG_JOIN_ACK: {
        const mesh_join_ack_payload_t *j = &f->u.join_ack;
        w_u8(c, j->status);
        w_u8(c, j->wifi_channel);
        w_u32(c, j->offline_timeout_ms);
        break;
    }
    case MESH_MSG_DATA: {
        const mesh_data_payload_t *d = &f->u.data;
        w_u8(c, d->backend);
        w_u8(c, d->status);
        w_u32(c, d->last_rtt_us);
        w_u8(c, d->channel_count);
        for (uint8_t i = 0; i < d->channel_count; i++) {
            w_u8(c, d->channels[i].quantity);
            w_u32(c, (uint32_t)d->channels[i].value_milli);
        }
        break;
    }
    case MESH_MSG_HEARTBEAT: {
        const mesh_heartbeat_payload_t *h = &f->u.hb;
        w_u32(c, h->uptime_s);
        w_u32(c, h->free_heap);
        w_u32(c, h->min_free_heap);
        w_u32(c, h->tx_ok);
        w_u32(c, h->tx_retries);
        w_u32(c, h->tx_failed);
        w_u32(c, h->queue_drops);
        w_u32(c, h->rtt_avg_us);
        w_u32(c, h->rtt_max_us);
        w_u8(c, (uint8_t)h->last_ack_rssi);
        w_u8(c, h->task_count);
        for (unsigned i = 0; i < MESH_HB_MAX_TASKS; i++) {
            w_u16(c, h->stack_hwm[i]);
        }
        break;
    }
    case MESH_MSG_ACK: {
        const mesh_ack_payload_t *a = &f->u.ack;
        w_u16(c, a->acked_seq);
        w_u8(c, a->acked_type);
        w_u8(c, a->acked_attempt);
        w_u8(c, a->status);
        w_u8(c, (uint8_t)a->rssi);
        break;
    }
    case MESH_MSG_CONFIG:
        w_u8(c, f->u.config.key);
        w_u32(c, f->u.config.value);
        break;
    default:
        c->overflow = true; /* unreachable: type validated by mesh_payload_size */
        break;
    }
}

mesh_err_t mesh_encode(const mesh_frame_t *f, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (f == NULL || buf == NULL || out_len == NULL) {
        return MESH_ERR_ARG;
    }
    size_t plen = 0;
    mesh_err_t err = mesh_payload_size(f, &plen);
    if (err != MESH_OK) {
        return err;
    }
    const size_t total = MESH_HEADER_SIZE + plen + MESH_TRAILER_SIZE;
    if (total > MESH_FRAME_MAX || total > cap) {
        return MESH_ERR_NO_SPACE;
    }

    wcur_t c = {.p = buf, .left = cap, .overflow = false};
    w_u8(&c, MESH_PROTO_MAGIC);
    w_u8(&c, MESH_PROTO_VERSION);
    w_u8(&c, f->hdr.type);
    w_u8(&c, f->hdr.flags);
    w_u16(&c, f->hdr.node_id);
    w_u16(&c, f->hdr.seq);
    w_u32(&c, f->hdr.uptime_ms);
    w_u8(&c, f->hdr.attempt);
    w_u8(&c, (uint8_t)plen);
    encode_payload(&c, f);
    if (c.overflow) {
        return MESH_ERR_NO_SPACE;
    }
    const uint16_t crc = mesh_crc16(buf, MESH_HEADER_SIZE + plen);
    w_u16(&c, crc);
    if (c.overflow) {
        return MESH_ERR_NO_SPACE;
    }
    *out_len = total;
    return MESH_OK;
}

mesh_err_t mesh_frame_set_attempt(uint8_t *buf, size_t len, uint8_t attempt)
{
    if (buf == NULL || len < MESH_FRAME_OVERHEAD) {
        return MESH_ERR_ARG;
    }
    buf[offsetof(mesh_header_t, attempt)] = attempt;
    const size_t body = len - MESH_TRAILER_SIZE;
    const uint16_t crc = mesh_crc16(buf, body);
    buf[body] = (uint8_t)(crc & 0xFFu);
    buf[body + 1] = (uint8_t)(crc >> 8);
    return MESH_OK;
}

mesh_err_t mesh_data_add(mesh_frame_t *f, mesh_quantity_t q, int32_t value_milli)
{
    if (f == NULL || f->hdr.type != MESH_MSG_DATA) {
        return MESH_ERR_ARG;
    }
    mesh_data_payload_t *d = &f->u.data;
    if (d->channel_count >= MESH_MAX_CHANNELS) {
        return MESH_ERR_NO_SPACE;
    }
    d->channels[d->channel_count].quantity = (uint8_t)q;
    d->channels[d->channel_count].value_milli = value_milli;
    d->channel_count++;
    return MESH_OK;
}

/* ------------------------------------------------------------------------- */
/* Decode                                                                    */
/* ------------------------------------------------------------------------- */

static mesh_err_t decode_payload(rcur_t *c, mesh_frame_t *f, size_t plen)
{
    switch ((mesh_msg_type_t)f->hdr.type) {
    case MESH_MSG_JOIN: {
        if (plen != sizeof(mesh_join_payload_t)) {
            return MESH_ERR_PAYLOAD;
        }
        mesh_join_payload_t *j = &f->u.join;
        j->fw_version = r_u32(c);
        j->boot_count = r_u32(c);
        j->report_interval_ms = r_u32(c);
        j->heartbeat_interval_ms = r_u32(c);
        j->backend = r_u8(c);
        j->reset_reason = r_u8(c);
        break;
    }
    case MESH_MSG_JOIN_ACK: {
        if (plen != sizeof(mesh_join_ack_payload_t)) {
            return MESH_ERR_PAYLOAD;
        }
        mesh_join_ack_payload_t *j = &f->u.join_ack;
        j->status = r_u8(c);
        j->wifi_channel = r_u8(c);
        j->offline_timeout_ms = r_u32(c);
        break;
    }
    case MESH_MSG_DATA: {
        if (plen < MESH_DATA_FIXED_SIZE) {
            return MESH_ERR_PAYLOAD;
        }
        mesh_data_payload_t *d = &f->u.data;
        d->backend = r_u8(c);
        d->status = r_u8(c);
        d->last_rtt_us = r_u32(c);
        d->channel_count = r_u8(c);
        if (d->channel_count > MESH_MAX_CHANNELS ||
            plen != MESH_DATA_FIXED_SIZE + (size_t)d->channel_count * sizeof(mesh_channel_t)) {
            return MESH_ERR_PAYLOAD;
        }
        for (uint8_t i = 0; i < d->channel_count; i++) {
            d->channels[i].quantity = r_u8(c);
            d->channels[i].value_milli = (int32_t)r_u32(c);
        }
        break;
    }
    case MESH_MSG_HEARTBEAT: {
        if (plen != sizeof(mesh_heartbeat_payload_t)) {
            return MESH_ERR_PAYLOAD;
        }
        mesh_heartbeat_payload_t *h = &f->u.hb;
        h->uptime_s = r_u32(c);
        h->free_heap = r_u32(c);
        h->min_free_heap = r_u32(c);
        h->tx_ok = r_u32(c);
        h->tx_retries = r_u32(c);
        h->tx_failed = r_u32(c);
        h->queue_drops = r_u32(c);
        h->rtt_avg_us = r_u32(c);
        h->rtt_max_us = r_u32(c);
        h->last_ack_rssi = (int8_t)r_u8(c);
        h->task_count = r_u8(c);
        for (unsigned i = 0; i < MESH_HB_MAX_TASKS; i++) {
            h->stack_hwm[i] = r_u16(c);
        }
        if (h->task_count > MESH_HB_MAX_TASKS) {
            return MESH_ERR_PAYLOAD;
        }
        break;
    }
    case MESH_MSG_ACK: {
        if (plen != sizeof(mesh_ack_payload_t)) {
            return MESH_ERR_PAYLOAD;
        }
        mesh_ack_payload_t *a = &f->u.ack;
        a->acked_seq = r_u16(c);
        a->acked_type = r_u8(c);
        a->acked_attempt = r_u8(c);
        a->status = r_u8(c);
        a->rssi = (int8_t)r_u8(c);
        break;
    }
    case MESH_MSG_CONFIG:
        if (plen != sizeof(mesh_config_payload_t)) {
            return MESH_ERR_PAYLOAD;
        }
        f->u.config.key = r_u8(c);
        f->u.config.value = r_u32(c);
        break;
    default:
        return MESH_ERR_TYPE;
    }
    return c->underflow ? MESH_ERR_PAYLOAD : MESH_OK;
}

mesh_err_t mesh_decode(const uint8_t *buf, size_t len, mesh_frame_t *out)
{
    if (buf == NULL || out == NULL) {
        return MESH_ERR_ARG;
    }
    if (len < MESH_FRAME_OVERHEAD) {
        return MESH_ERR_TOO_SHORT;
    }
    if (buf[0] != MESH_PROTO_MAGIC) {
        return MESH_ERR_MAGIC;
    }
    if (buf[1] != MESH_PROTO_VERSION) {
        return MESH_ERR_VERSION;
    }
    const size_t plen = buf[offsetof(mesh_header_t, payload_len)];
    if (len > MESH_FRAME_MAX || len != MESH_FRAME_OVERHEAD + plen) {
        return MESH_ERR_LENGTH;
    }
    const size_t body = MESH_HEADER_SIZE + plen;
    const uint16_t rx_crc = (uint16_t)(buf[body] | (uint16_t)(buf[body + 1] << 8));
    if (mesh_crc16(buf, body) != rx_crc) {
        return MESH_ERR_CRC;
    }

    memset(out, 0, sizeof(*out));
    rcur_t c = {.p = buf, .left = body, .underflow = false};
    out->hdr.magic = r_u8(&c);
    out->hdr.version = r_u8(&c);
    out->hdr.type = r_u8(&c);
    out->hdr.flags = r_u8(&c);
    out->hdr.node_id = r_u16(&c);
    out->hdr.seq = r_u16(&c);
    out->hdr.uptime_ms = r_u32(&c);
    out->hdr.attempt = r_u8(&c);
    out->hdr.payload_len = r_u8(&c);
    return decode_payload(&c, out, plen);
}

/* ------------------------------------------------------------------------- */
/* Strings                                                                   */
/* ------------------------------------------------------------------------- */

const char *mesh_err_str(mesh_err_t err)
{
    switch (err) {
    case MESH_OK: return "ok";
    case MESH_ERR_ARG: return "bad_argument";
    case MESH_ERR_TOO_SHORT: return "too_short";
    case MESH_ERR_MAGIC: return "bad_magic";
    case MESH_ERR_VERSION: return "bad_version";
    case MESH_ERR_LENGTH: return "bad_length";
    case MESH_ERR_CRC: return "bad_crc";
    case MESH_ERR_TYPE: return "bad_type";
    case MESH_ERR_PAYLOAD: return "bad_payload";
    case MESH_ERR_NO_SPACE: return "no_space";
    default: return "unknown";
    }
}

const char *mesh_msg_type_str(uint8_t type)
{
    switch ((mesh_msg_type_t)type) {
    case MESH_MSG_JOIN: return "join";
    case MESH_MSG_JOIN_ACK: return "join_ack";
    case MESH_MSG_DATA: return "data";
    case MESH_MSG_HEARTBEAT: return "heartbeat";
    case MESH_MSG_ACK: return "ack";
    case MESH_MSG_CONFIG: return "config";
    default: return "unknown";
    }
}

const char *mesh_quantity_key(uint8_t quantity)
{
    static const char *const keys[MESH_Q_COUNT_] = {
        [MESH_Q_GAS_ADC_MV] = "gas_mv",
        [MESH_Q_GAS_RS_R0] = "gas_rs_r0",
        [MESH_Q_GAS_PPM] = "gas_ppm",
        [MESH_Q_MOTOR_RPM] = "motor_rpm",
        [MESH_Q_MOTOR_ANGLE] = "motor_angle_deg",
        [MESH_Q_MOTOR_DUTY] = "motor_duty_pct",
        [MESH_Q_ENC_STATUS] = "enc_status",
        [MESH_Q_ENC_AGC] = "enc_agc",
        [MESH_Q_ACCEL_X] = "accel_x_g",
        [MESH_Q_ACCEL_Y] = "accel_y_g",
        [MESH_Q_ACCEL_Z] = "accel_z_g",
        [MESH_Q_GYRO_X] = "gyro_x_dps",
        [MESH_Q_GYRO_Y] = "gyro_y_dps",
        [MESH_Q_GYRO_Z] = "gyro_z_dps",
        [MESH_Q_IMU_TEMP] = "imu_temp_c",
        [MESH_Q_PITCH] = "pitch_deg",
        [MESH_Q_ROLL] = "roll_deg",
    };
    if (quantity == 0 || quantity >= MESH_Q_COUNT_) {
        return NULL;
    }
    return keys[quantity];
}
