/*
 * SPDX-License-Identifier: MIT
 */
#include "gw_json.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gw_fsm.h"
#include "mesh_util.h"

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} jw_t;

static void jw_printf(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void jw_printf(jw_t *w, const char *fmt, ...)
{
    if (w->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->overflow = true;
        return;
    }
    w->len += (size_t)n;
}

int gw_json_milli(int32_t v, char *buf, size_t cap)
{
    const int64_t x = v;
    const uint64_t mag = (uint64_t)(x < 0 ? -x : x);
    const uint64_t ip = mag / 1000u;
    uint32_t frac = (uint32_t)(mag % 1000u);
    const char *sign = (x < 0) ? "-" : "";
    int n;
    if (frac == 0) {
        n = snprintf(buf, cap, "%s%" PRIu64, sign, ip);
    } else {
        int digits = 3;
        while (frac % 10u == 0) {
            frac /= 10u;
            digits--;
        }
        n = snprintf(buf, cap, "%s%" PRIu64 ".%0*" PRIu32, sign, ip, digits, frac);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static void jw_milli(jw_t *w, int32_t v)
{
    char tmp[24];
    if (gw_json_milli(v, tmp, sizeof(tmp)) < 0) {
        w->overflow = true;
        return;
    }
    jw_printf(w, "%s", tmp);
}

static void head(jw_t *w, const gw_out_t *r, const char *type)
{
    jw_printf(w, "{\"ts\":%" PRIu64 ",\"type\":\"%s\"", r->ts_ms, type);
}

static void node_field(jw_t *w, const gw_out_t *r)
{
    jw_printf(w, ",\"node\":%u", (unsigned)r->node_id);
}

static void mac_field(jw_t *w, const gw_out_t *r)
{
    char macs[MESH_MAC_STR_LEN];
    mesh_format_mac(r->mac, macs);
    jw_printf(w, ",\"mac\":\"%s\"", macs);
}

static void frame_meta(jw_t *w, const gw_out_t *r)
{
    jw_printf(w, ",\"seq\":%u,\"att\":%u,\"rssi\":%d", (unsigned)r->seq, (unsigned)r->attempt, (int)r->rssi);
}

static void backend_list(jw_t *w, uint8_t mask)
{
    static const struct {
        uint8_t bit;
        const char *name;
    } names[] = {{MESH_BACKEND_SIM, "sim"},
                 {MESH_BACKEND_MQ2, "mq2"},
                 {MESH_BACKEND_MOTOR, "motor_as5600"},
                 {MESH_BACKEND_MPU6050, "mpu6050"}};
    jw_printf(w, "[");
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (mask & names[i].bit) {
            jw_printf(w, "%s\"%s\"", first ? "" : ",", names[i].name);
            first = false;
        }
    }
    jw_printf(w, "]");
}

static const char *seq_result_str(uint8_t r)
{
    switch ((mesh_seq_result_t)r) {
    case MESH_SEQ_NEW: return "new";
    case MESH_SEQ_LATE: return "late";
    case MESH_SEQ_DUPLICATE: return "dup";
    case MESH_SEQ_RESET: return "reset";
    default: return "?";
    }
}

static const char *join_status_str(uint8_t s)
{
    switch ((mesh_join_status_t)s) {
    case MESH_JOIN_ACCEPTED: return "accepted";
    case MESH_JOIN_REJECTED_FULL: return "rejected_full";
    case MESH_JOIN_REJECTED_VERSION: return "rejected_version";
    case MESH_JOIN_REJECTED_ID_CONFLICT: return "rejected_id_conflict";
    default: return "?";
    }
}

static const char *ack_status_str(uint8_t s)
{
    switch (s) {
    case MESH_ACK_OK: return "ok";
    case MESH_ACK_DUPLICATE: return "duplicate";
    case MESH_ACK_BUSY: return "busy";
    case MESH_ACK_REJECTED: return "rejected";
    case MESH_ACK_INVALID: return "invalid";
    case 0xFF: return "no_ack";
    default: return "?";
    }
}

static const char *config_key_str(uint8_t k)
{
    switch ((mesh_config_key_t)k) {
    case MESH_CFG_REPORT_INTERVAL_MS: return "report_ms";
    case MESH_CFG_HEARTBEAT_INTERVAL_MS: return "hb_ms";
    case MESH_CFG_REBOOT: return "reboot";
    default: return "?";
    }
}

static const char *rx_err_str(int16_t e)
{
    switch (e) {
    case GW_RXERR_UNKNOWN_NODE: return "unknown_node";
    case GW_RXERR_BAD_DIRECTION: return "bad_direction";
    case GW_RXERR_BUSY: return "busy";
    default: return mesh_err_str((mesh_err_t)e);
    }
}

static void fw_string(jw_t *w, uint32_t v)
{
    jw_printf(w, "\"%u.%u.%u\"", (unsigned)((v >> 16) & 0xFF), (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
}

static void fmt_data(jw_t *w, const gw_out_t *r)
{
    const mesh_data_payload_t *d = &r->u.data;
    jw_printf(w, ",\"gap\":%" PRIu32 ",\"seq_state\":\"%s\",\"up_ms\":%" PRIu32 ",\"rtt_us\":%" PRIu32
                 ",\"status\":%u,\"backend\":",
              r->gap, seq_result_str(r->seq_result), r->node_uptime_ms, d->last_rtt_us, (unsigned)d->status);
    backend_list(w, d->backend);
    jw_printf(w, ",\"values\":{");
    for (uint8_t i = 0; i < d->channel_count && i < MESH_MAX_CHANNELS; i++) {
        const char *key = mesh_quantity_key(d->channels[i].quantity);
        if (key != NULL) {
            jw_printf(w, "%s\"%s\":", i ? "," : "", key);
        } else {
            jw_printf(w, "%s\"q%u\":", i ? "," : "", (unsigned)d->channels[i].quantity);
        }
        jw_milli(w, d->channels[i].value_milli);
    }
    jw_printf(w, "}");
}

static void fmt_hb(jw_t *w, const gw_out_t *r)
{
    const mesh_heartbeat_payload_t *h = &r->u.hb;
    jw_printf(w,
              ",\"gap\":%" PRIu32 ",\"seq_state\":\"%s\",\"up_s\":%" PRIu32 ",\"heap\":%" PRIu32
              ",\"min_heap\":%" PRIu32 ",\"tx_ok\":%" PRIu32 ",\"retries\":%" PRIu32 ",\"tx_fail\":%" PRIu32
              ",\"q_drops\":%" PRIu32 ",\"rtt_avg_us\":%" PRIu32 ",\"rtt_max_us\":%" PRIu32 ",\"ack_rssi\":%d,\"hwm\":[",
              r->gap, seq_result_str(r->seq_result), h->uptime_s, h->free_heap, h->min_free_heap, h->tx_ok,
              h->tx_retries, h->tx_failed, h->queue_drops, h->rtt_avg_us, h->rtt_max_us, (int)h->last_ack_rssi);
    for (uint8_t i = 0; i < h->task_count && i < MESH_HB_MAX_TASKS; i++) {
        jw_printf(w, "%s%u", i ? "," : "", (unsigned)h->stack_hwm[i]);
    }
    jw_printf(w, "]");
}

static void fmt_stats(jw_t *w, const gw_out_t *r)
{
    const gw_stats_t *s = &r->u.stats;
    jw_printf(w,
              ",\"up_s\":%" PRIu32 ",\"rx\":%" PRIu32 ",\"rx_err\":%" PRIu32 ",\"rx_q_drops\":%" PRIu32
              ",\"out_q_drops\":%" PRIu32 ",\"out_q_peak\":%" PRIu32 ",\"acks\":%" PRIu32 ",\"busy\":%" PRIu32
              ",\"unknown\":%" PRIu32 ",\"dups\":%" PRIu32 ",\"heap\":%" PRIu32 ",\"min_heap\":%" PRIu32
              ",\"psram\":%" PRIu32 ",\"nodes\":{\"total\":%u,\"online\":%u,\"suspect\":%u,\"offline\":%u}",
              s->uptime_s, s->rx_frames, s->rx_errors, s->rx_queue_drops, s->out_queue_drops, s->out_queue_peak,
              s->acks_sent, s->busy_acks, s->unknown_node, s->duplicates, s->free_heap, s->min_free_heap,
              s->free_psram, s->nodes_total, s->nodes_online, s->nodes_suspect, s->nodes_offline);
    jw_printf(w, ",\"hwm\":{");
    for (uint8_t i = 0; i < s->task_count && i < GW_MAX_TASKS; i++) {
        jw_printf(w, "%s\"%s\":%u", i ? "," : "", s->task_names[i] ? s->task_names[i] : "?",
                  (unsigned)s->stack_hwm[i]);
    }
    jw_printf(w, "}");
}

int gw_json_format(const gw_out_t *r, char *buf, size_t cap)
{
    if (r == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    jw_t w = {.buf = buf, .cap = cap, .len = 0, .overflow = false};
    buf[0] = '\0';
    switch (r->kind) {
    case GW_OUT_BOOT:
        head(&w, r, "boot");
        jw_printf(&w, ",\"schema\":%d,\"proto\":%u,\"fw\":", GW_JSON_SCHEMA_VERSION, (unsigned)MESH_PROTO_VERSION);
        fw_string(&w, r->u.boot.fw_version);
        mac_field(&w, r);
        jw_printf(&w, ",\"channel\":%u,\"encryption\":%s,\"capacity\":%u,\"restored\":%d",
                  (unsigned)r->u.boot.channel, r->u.boot.encryption ? "true" : "false",
                  (unsigned)r->u.boot.capacity, (int)r->u.boot.restored);
        break;
    case GW_OUT_GW_STATE:
        head(&w, r, "gw_state");
        jw_printf(&w, ",\"from\":\"%s\",\"to\":\"%s\",\"event\":\"%s\"", gw_state_str((gw_state_t)r->u.gw_state.from),
                  gw_state_str((gw_state_t)r->u.gw_state.to), gw_event_str((gw_event_t)r->u.gw_state.event));
        break;
    case GW_OUT_JOIN:
        head(&w, r, "join");
        node_field(&w, r);
        mac_field(&w, r);
        frame_meta(&w, r);
        jw_printf(&w, ",\"gap\":%" PRIu32 ",\"seq_state\":\"%s\",\"status\":\"%s\",\"new\":%s,\"fw\":", r->gap,
                  seq_result_str(r->seq_result), join_status_str(r->u.join.status),
                  r->u.join.is_new ? "true" : "false");
        fw_string(&w, r->u.join.p.fw_version);
        jw_printf(&w, ",\"boot\":%" PRIu32 ",\"reset\":%u,\"report_ms\":%" PRIu32 ",\"hb_ms\":%" PRIu32 ",\"backend\":",
                  r->u.join.p.boot_count, (unsigned)r->u.join.p.reset_reason, r->u.join.p.report_interval_ms,
                  r->u.join.p.heartbeat_interval_ms);
        backend_list(&w, r->u.join.p.backend);
        break;
    case GW_OUT_DATA:
        head(&w, r, "data");
        node_field(&w, r);
        frame_meta(&w, r);
        fmt_data(&w, r);
        break;
    case GW_OUT_HEARTBEAT:
        head(&w, r, "hb");
        node_field(&w, r);
        frame_meta(&w, r);
        fmt_hb(&w, r);
        break;
    case GW_OUT_DUP:
        head(&w, r, "dup");
        node_field(&w, r);
        frame_meta(&w, r);
        break;
    case GW_OUT_NODE_STATE:
        head(&w, r, "node_state");
        node_field(&w, r);
        jw_printf(&w, ",\"from\":\"%s\",\"to\":\"%s\",\"silent_ms\":%" PRIu32,
                  gw_node_state_str(r->u.node_state.from), gw_node_state_str(r->u.node_state.to),
                  r->u.node_state.silent_ms);
        break;
    case GW_OUT_RX_ERROR:
        head(&w, r, "rx_error");
        if (r->node_id != MESH_BROADCAST_NODE_ID) {
            node_field(&w, r);
        }
        mac_field(&w, r);
        jw_printf(&w, ",\"err\":\"%s\",\"len\":%u", rx_err_str(r->u.rx_error.err), (unsigned)r->u.rx_error.len);
        break;
    case GW_OUT_LINK: {
        const mesh_seq_tracker_t *t = &r->u.link.seq;
        head(&w, r, "link");
        node_field(&w, r);
        mac_field(&w, r);
        jw_printf(&w,
                  ",\"state\":\"%s\",\"rx\":%" PRIu32 ",\"lost\":%" PRIu32 ",\"dup\":%" PRIu32 ",\"late\":%" PRIu32
                  ",\"resets\":%" PRIu32 ",\"loss_ppm\":%" PRIu32 ",\"rx_err\":%" PRIu32 ",\"rssi\":%d",
                  gw_node_state_str((gw_node_state_t)r->u.link.state), t->received, t->lost, t->duplicates,
                  t->reordered, t->resets, mesh_seq_loss_ppm(t), r->u.link.rx_errors, (int)r->u.link.rssi);
        break;
    }
    case GW_OUT_GW_STATS:
        head(&w, r, "gw_stats");
        fmt_stats(&w, r);
        break;
    case GW_OUT_CONFIG:
        head(&w, r, "config");
        node_field(&w, r);
        jw_printf(&w, ",\"key\":\"%s\",\"value\":%" PRIu32 ",\"result\":\"%s\",\"attempts\":%u",
                  config_key_str(r->u.config.key), r->u.config.value, ack_status_str(r->u.config.status),
                  (unsigned)r->u.config.attempts);
        break;
    case GW_OUT_INFO: {
        head(&w, r, "info");
        jw_printf(&w, ",\"msg\":\"");
        /* Escape the free-text message. */
        for (size_t i = 0; i < sizeof(r->u.info) && r->u.info[i] != '\0'; i++) {
            const char c = r->u.info[i];
            if (c == '"' || c == '\\') {
                jw_printf(&w, "\\%c", c);
            } else if ((unsigned char)c < 0x20) {
                jw_printf(&w, "\\u%04x", (unsigned)c);
            } else {
                jw_printf(&w, "%c", c);
            }
        }
        jw_printf(&w, "\"");
        break;
    }
    default:
        head(&w, r, "unknown");
        break;
    }
    jw_printf(&w, "}\n");
    return w.overflow ? -1 : (int)w.len;
}
