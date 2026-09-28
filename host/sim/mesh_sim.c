/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_sim.c - Deterministic discrete-event simulator of the ESP-NOW mesh.
 *
 * What is REAL (the exact firmware sources, compiled for the host):
 *   - frame codec + CRC (mesh_protocol), stop-and-wait retry FSM (mesh_tx),
 *     sequence tracking (mesh_seq)
 *   - the gateway's per-frame decision procedure, registry, liveness state
 *     machine and JSON Lines emitter (gw_core)
 *   - node configuration handling (node_config) and the simulated sensor
 *     backend (sensor_sim)
 *
 * What is MODELLED (replaces hardware / FreeRTOS scheduling):
 *   - the radio: per-node Gilbert-Elliott loss process (bursty losses),
 *     802.11b 1 Mbit/s airtime, gateway processing delay, RSSI
 *   - the node's task loop (same algorithm as node_tasks.c, event driven)
 *   - the gateway's serial port (921600 baud) and bounded output queue
 *
 * Output: gateway JSON Lines (same schema as the firmware) plus a ground-truth
 * JSON summary used to validate the dashboard's metrics. Every random choice
 * comes from xorshift32 streams derived from --seed, so a run is bit-for-bit
 * reproducible.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_core.h"
#include "gw_fsm.h"
#include "gw_json.h"
#include "mesh_protocol.h"
#include "mesh_rand.h"
#include "mesh_tx.h"
#include "mesh_util.h"
#include "node_config.h"
#include "sensor_sim.h"

#define MAX_NODES 16
#define NODE_Q_CAP 8
#define GW_OUT_CAP 256
#define JOIN_TIMEOUT_US 300000u
#define GW_TICK_US 500000u
#define BIT_US_1MBPS 1u
#define PHY_OVERHEAD_US 192u   /* long preamble + PLCP header at 1 Mbit/s */
#define MAC_OVERHEAD_B 43u     /* 802.11 header + vendor action frame + ESP-NOW header + FCS */
#define MAC_ACK_US 304u        /* SIFS + 14-byte ACK at 1 Mbit/s incl. preamble */

/* ------------------------------------------------------------------------- */
/* Options                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    int nodes;
    double duration_s;
    uint32_t seed;
    uint32_t baud;
    uint32_t report_ms;
    uint32_t hb_ms;
    const char *out_path;
    const char *truth_path;
    bool scenario_events;
} opts_t;

/* ------------------------------------------------------------------------- */
/* Event queue (binary min-heap on time, FIFO on ties)                       */
/* ------------------------------------------------------------------------- */

typedef enum {
    EV_NODE_SENSE,
    EV_NODE_HB,
    EV_NODE_TIMER,
    EV_NODE_SEND_DONE,
    EV_FRAME_TO_GW,
    EV_FRAME_TO_NODE,
    EV_NODE_POWER,
    EV_NODE_INTERFERENCE,
    EV_GW_TICK,
    EV_GW_STATS,
    EV_GW_CMD,
    EV_GW_CMD_TIMER,
    EV_GW_CMD_SEND_DONE,
} ev_kind_t;

typedef struct {
    uint64_t t;
    uint64_t order;
    ev_kind_t kind;
    int node;
    uint32_t gen;
    uint32_t arg;
    bool ok;
    uint8_t len;
    uint8_t data[MESH_FRAME_MAX];
} event_t;

static event_t *s_heap;
static size_t s_heap_len, s_heap_cap;
static uint64_t s_order;

static bool ev_less(const event_t *a, const event_t *b)
{
    return a->t < b->t || (a->t == b->t && a->order < b->order);
}

static void ev_push(event_t e)
{
    if (s_heap_len == s_heap_cap) {
        s_heap_cap = s_heap_cap ? s_heap_cap * 2 : 1024;
        s_heap = realloc(s_heap, s_heap_cap * sizeof(event_t));
        if (s_heap == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
    e.order = s_order++;
    size_t i = s_heap_len++;
    s_heap[i] = e;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!ev_less(&s_heap[i], &s_heap[p])) {
            break;
        }
        event_t tmp = s_heap[i];
        s_heap[i] = s_heap[p];
        s_heap[p] = tmp;
        i = p;
    }
}

static event_t ev_pop(void)
{
    event_t top = s_heap[0];
    s_heap[0] = s_heap[--s_heap_len];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < s_heap_len && ev_less(&s_heap[l], &s_heap[m])) {
            m = l;
        }
        if (r < s_heap_len && ev_less(&s_heap[r], &s_heap[m])) {
            m = r;
        }
        if (m == i) {
            break;
        }
        event_t tmp = s_heap[i];
        s_heap[i] = s_heap[m];
        s_heap[m] = tmp;
        i = m;
    }
    return top;
}

static event_t ev_make(uint64_t t, ev_kind_t kind, int node)
{
    event_t e;
    memset(&e, 0, sizeof(e));
    e.t = t;
    e.kind = kind;
    e.node = node;
    return e;
}

/* ------------------------------------------------------------------------- */
/* Channel model                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool bad;
    double p_gb, p_bg;          /* transition probabilities per frame */
    double loss_good, loss_bad; /* loss probability in each state */
    double forced_loss;         /* > 0 during a scripted interference burst */
    int rssi_base;
    uint32_t rng;
} channel_t;

/** Advance the Markov chain and decide the fate of one frame. */
static bool channel_lost(channel_t *c)
{
    const double u = mesh_rand_unit(&c->rng);
    c->bad = c->bad ? (u >= c->p_bg) : (u < c->p_gb);
    if (c->forced_loss > 0.0) {
        return mesh_rand_unit(&c->rng) < c->forced_loss;
    }
    return mesh_rand_unit(&c->rng) < (c->bad ? c->loss_bad : c->loss_good);
}

static int8_t channel_rssi(channel_t *c)
{
    const int noise = (int)mesh_rand_below(&c->rng, 7) - 3;
    int v = c->rssi_base + noise - (c->bad ? 8 : 0);
    if (v < -95) {
        v = -95;
    }
    return (int8_t)v;
}

static uint32_t airtime_us(size_t len)
{
    return PHY_OVERHEAD_US + (uint32_t)(len + MAC_OVERHEAD_B) * 8u * BIT_US_1MBPS;
}

/* ------------------------------------------------------------------------- */
/* Node model (mirrors sensor_node/main/node_tasks.c)                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool is_hb;
    sensor_reading_t reading;
} q_item_t;

typedef struct {
    uint64_t readings;       /* sensor samples produced */
    uint64_t queue_drops;    /* dropped by the drop-oldest policy */
    uint64_t data_sent;      /* DATA frames that entered delivery (seq allocated) */
    uint64_t data_acked;     /* DATA frames acknowledged (OK or DUPLICATE) */
    uint64_t data_failed;    /* DATA frames abandoned after max attempts */
    uint64_t hb_sent, hb_acked, hb_failed;
    uint64_t attempts;       /* radio transmissions of DATA/HB frames */
    uint64_t joins_sent;
    uint64_t rejoins;
    uint64_t busy_acks;
} truth_t;

typedef struct {
    int idx;
    uint8_t mac[MESH_MAC_LEN];
    node_config_t cfg;
    bool powered;
    uint64_t power_on_us;
    uint32_t boot_count;
    channel_t ch;
    sensor_sim_ctx_t sim;

    bool joined;
    bool join_pending;
    uint64_t join_deadline_us;
    uint64_t next_join_us;
    uint8_t join_attempt;
    uint32_t join_rng;

    q_item_t q[NODE_Q_CAP];
    int q_head, q_len;

    mesh_tx_t tx;
    bool busy;
    bool cur_is_hb;
    uint8_t frame[MESH_FRAME_MAX];
    size_t frame_len;
    uint16_t seq;
    uint32_t consecutive_fail;
    uint32_t timer_gen;
    uint32_t sense_gen, hb_gen;

    /* node stats (as carried in heartbeats) */
    uint32_t tx_ok, tx_retries, tx_failed, q_drops;
    uint64_t rtt_sum;
    uint32_t rtt_cnt, rtt_max, last_rtt;
    int8_t last_ack_rssi;

    truth_t truth;
} node_t;

static opts_t O;
static node_t s_nodes[MAX_NODES];
static uint64_t s_now;
static FILE *s_out;

/* --- gateway model --- */
static gw_core_t s_gw;
static gw_fsm_t s_fsm;
static uint64_t s_serial_busy_until;
static uint64_t s_out_finish[GW_OUT_CAP];
static int s_out_head, s_out_len;
static uint32_t s_out_drops, s_out_peak;
static uint64_t s_lines, s_bytes;

static const mesh_retry_policy_t NODE_POLICY = {
    .max_attempts = 4, .ack_timeout_us = 30000, .backoff_base_us = 20000, .backoff_max_us = 200000,
    .jitter_us = 10000};
static const mesh_retry_policy_t JOIN_POLICY = {
    .max_attempts = 8, .ack_timeout_us = 1, .backoff_base_us = 500000, .backoff_max_us = 30000000,
    .jitter_us = 250000};

static uint32_t now_ms(void)
{
    return (uint32_t)(s_now / 1000u);
}

/* Serial port model: each line occupies the UART for len * 10 bit-times. */
static void out_expire(void)
{
    while (s_out_len > 0 && s_out_finish[s_out_head] <= s_now) {
        s_out_head = (s_out_head + 1) % GW_OUT_CAP;
        s_out_len--;
    }
}

static bool out_can_forward(void)
{
    out_expire();
    return s_out_len + GW_RX_MAX_OUT <= GW_OUT_CAP;
}

static void emit(const gw_out_t *rec)
{
    out_expire();
    if (s_out_len >= GW_OUT_CAP) {
        s_out_drops++;
        return;
    }
    char line[GW_JSON_LINE_MAX];
    const int n = gw_json_format(rec, line, sizeof(line));
    if (n <= 0) {
        return;
    }
    fwrite(line, 1, (size_t)n, s_out);
    s_lines++;
    s_bytes += (uint64_t)n;
    const uint64_t start = s_serial_busy_until > s_now ? s_serial_busy_until : s_now;
    s_serial_busy_until = start + (uint64_t)n * 10u * 1000000u / O.baud;
    s_out_finish[(s_out_head + s_out_len) % GW_OUT_CAP] = s_serial_busy_until;
    s_out_len++;
    if ((uint32_t)s_out_len > s_out_peak) {
        s_out_peak = (uint32_t)s_out_len;
    }
}

static void emit_gw_state(gw_event_t ev)
{
    gw_state_t from;
    if (gw_fsm_dispatch(&s_fsm, ev, &from)) {
        gw_out_t r;
        memset(&r, 0, sizeof(r));
        r.kind = GW_OUT_GW_STATE;
        r.ts_ms = now_ms();
        r.u.gw_state.from = (uint8_t)from;
        r.u.gw_state.to = (uint8_t)s_fsm.state;
        r.u.gw_state.event = (uint8_t)ev;
        emit(&r);
    }
}

static void emit_info(const char *msg)
{
    gw_out_t r;
    memset(&r, 0, sizeof(r));
    r.kind = GW_OUT_INFO;
    r.ts_ms = now_ms();
    snprintf(r.u.info, sizeof(r.u.info), "%s", msg);
    emit(&r);
}

/* ---------------------------- node helpers ------------------------------- */

static void node_arm_timer(node_t *n, uint64_t at)
{
    event_t e = ev_make(at, EV_NODE_TIMER, n->idx);
    e.gen = ++n->timer_gen;
    ev_push(e);
}

/** Radio transmission from a node; schedules send-done and (maybe) arrival. */
static void node_radio_send(node_t *n, const uint8_t *buf, size_t len, bool broadcast, bool want_send_done)
{
    const uint32_t air = airtime_us(len);
    const bool lost = channel_lost(&n->ch);
    if (!lost) {
        event_t a = ev_make(s_now + air, EV_FRAME_TO_GW, n->idx);
        a.len = (uint8_t)len;
        memcpy(a.data, buf, len);
        a.arg = (uint32_t)(int32_t)channel_rssi(&n->ch);
        ev_push(a);
    }
    if (want_send_done) {
        /* Unicast: MAC-layer ACK decides success; broadcast always "succeeds". */
        event_t d = ev_make(s_now + air + (broadcast ? 0 : MAC_ACK_US), EV_NODE_SEND_DONE, n->idx);
        d.ok = broadcast || !lost;
        d.gen = n->timer_gen;
        ev_push(d);
    }
}

static void node_kick(node_t *n);

static void node_enqueue(node_t *n, const q_item_t *it, bool front)
{
    if (n->q_len == NODE_Q_CAP) {
        n->q_head = (n->q_head + 1) % NODE_Q_CAP; /* drop oldest */
        n->q_len--;
        n->q_drops++;
        n->truth.queue_drops++;
    }
    if (front) {
        n->q_head = (n->q_head + NODE_Q_CAP - 1) % NODE_Q_CAP;
        n->q[n->q_head] = *it;
    } else {
        n->q[(n->q_head + n->q_len) % NODE_Q_CAP] = *it;
    }
    n->q_len++;
    node_kick(n);
}

static void node_send_join(node_t *n)
{
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_JOIN, n->cfg.node_id, n->seq++, (uint32_t)((s_now - n->power_on_us) / 1000u), 0);
    f.u.join.fw_version = mesh_fw_version(1, 0, 0);
    f.u.join.boot_count = n->boot_count;
    f.u.join.report_interval_ms = n->cfg.report_interval_ms;
    f.u.join.heartbeat_interval_ms = n->cfg.heartbeat_interval_ms;
    f.u.join.backend = MESH_BACKEND_SIM;
    f.u.join.reset_reason = n->boot_count > 1 ? 1 : 0;
    uint8_t buf[MESH_FRAME_MAX];
    size_t len;
    if (mesh_encode(&f, buf, sizeof(buf), &len) != MESH_OK) {
        return;
    }
    n->truth.joins_sent++;
    node_radio_send(n, buf, len, true, false);
    n->join_pending = true;
    n->join_deadline_us = s_now + JOIN_TIMEOUT_US;
    node_arm_timer(n, n->join_deadline_us);
}

static void node_start_delivery(node_t *n)
{
    const q_item_t it = n->q[n->q_head];
    n->q_head = (n->q_head + 1) % NODE_Q_CAP;
    n->q_len--;

    mesh_frame_t f;
    const uint16_t seq = n->seq++;
    const uint32_t up = (uint32_t)((s_now - n->power_on_us) / 1000u);
    if (it.is_hb) {
        mesh_frame_init(&f, MESH_MSG_HEARTBEAT, n->cfg.node_id, seq, up, MESH_FLAG_ACK_REQ);
        f.u.hb.uptime_s = up / 1000u;
        f.u.hb.tx_ok = n->tx_ok;
        f.u.hb.tx_retries = n->tx_retries;
        f.u.hb.tx_failed = n->tx_failed;
        f.u.hb.queue_drops = n->q_drops;
        f.u.hb.rtt_avg_us = n->rtt_cnt ? (uint32_t)(n->rtt_sum / n->rtt_cnt) : 0;
        f.u.hb.rtt_max_us = n->rtt_max;
        f.u.hb.last_ack_rssi = n->last_ack_rssi;
        f.u.hb.task_count = 0; /* no task stacks in simulation */
        n->rtt_sum = 0;
        n->rtt_cnt = 0;
        n->rtt_max = 0;
        n->truth.hb_sent++;
    } else {
        mesh_frame_init(&f, MESH_MSG_DATA, n->cfg.node_id, seq, up, MESH_FLAG_ACK_REQ);
        f.u.data.backend = it.reading.backend_mask;
        f.u.data.status = it.reading.status;
        f.u.data.last_rtt_us = n->last_rtt;
        f.u.data.channel_count = it.reading.count;
        memcpy(f.u.data.channels, it.reading.ch, sizeof(mesh_channel_t) * it.reading.count);
        n->truth.data_sent++;
    }
    if (mesh_encode(&f, n->frame, sizeof(n->frame), &n->frame_len) != MESH_OK) {
        return;
    }
    n->cur_is_hb = it.is_hb;
    n->busy = true;
    (void)mesh_tx_start(&n->tx, seq);
    mesh_frame_set_attempt(n->frame, n->frame_len, n->tx.attempt);
    n->truth.attempts++;
    n->timer_gen++;
    node_radio_send(n, n->frame, n->frame_len, false, true);
}

static void node_retransmit(node_t *n)
{
    mesh_frame_set_attempt(n->frame, n->frame_len, n->tx.attempt);
    n->tx_retries++;
    n->truth.attempts++;
    n->timer_gen++;
    node_radio_send(n, n->frame, n->frame_len, false, true);
}

static void node_finish(node_t *n, bool delivered)
{
    n->busy = false;
    if (delivered) {
        n->tx_ok++;
        n->last_rtt = n->tx.rtt_us;
        n->rtt_sum += n->tx.rtt_us;
        n->rtt_cnt++;
        if (n->tx.rtt_us > n->rtt_max) {
            n->rtt_max = n->tx.rtt_us;
        }
        n->consecutive_fail = 0;
        if (n->cur_is_hb) {
            n->truth.hb_acked++;
        } else {
            n->truth.data_acked++;
        }
        if (n->tx.acked_status == MESH_ACK_REJECTED) {
            n->joined = false;
            n->truth.rejoins++;
        }
    } else {
        n->tx_failed++;
        if (n->cur_is_hb) {
            n->truth.hb_failed++;
        } else {
            n->truth.data_failed++;
        }
        if (++n->consecutive_fail >= 3) {
            n->joined = false; /* gateway lost: re-join immediately */
            n->truth.rejoins++;
            n->join_attempt = 1;
            n->next_join_us = s_now;
        }
    }
    node_kick(n);
}

static void node_handle_action(node_t *n, mesh_tx_action_t act)
{
    switch (act) {
    case MESH_TX_ACTION_SEND:
        node_retransmit(n);
        break;
    case MESH_TX_ACTION_DONE:
        node_finish(n, true);
        break;
    case MESH_TX_ACTION_FAILED:
        node_finish(n, false);
        break;
    case MESH_TX_ACTION_WAIT:
    default:
        node_arm_timer(n, mesh_tx_deadline(&n->tx));
        break;
    }
}

static void node_kick(node_t *n)
{
    if (!n->powered || n->busy || n->join_pending) {
        return;
    }
    if (!n->joined) {
        if (s_now >= n->next_join_us) {
            node_send_join(n); /* otherwise the join backoff timer will kick us */
        }
        return;
    }
    if (n->q_len > 0) {
        node_start_delivery(n);
    }
}

static void node_send_ack(node_t *n, const mesh_frame_t *in, uint8_t status, int8_t rssi)
{
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_ACK, n->cfg.node_id, n->seq++, (uint32_t)((s_now - n->power_on_us) / 1000u), 0);
    f.u.ack.acked_seq = in->hdr.seq;
    f.u.ack.acked_type = in->hdr.type;
    f.u.ack.acked_attempt = in->hdr.attempt;
    f.u.ack.status = status;
    f.u.ack.rssi = rssi;
    uint8_t buf[MESH_FRAME_MAX];
    size_t len;
    if (mesh_encode(&f, buf, sizeof(buf), &len) == MESH_OK) {
        node_radio_send(n, buf, len, false, false);
    }
}

static void node_on_frame(node_t *n, const uint8_t *data, size_t len, int8_t rssi)
{
    mesh_frame_t f;
    if (!n->powered || mesh_decode(data, len, &f) != MESH_OK || f.hdr.node_id != MESH_GATEWAY_NODE_ID) {
        return;
    }
    switch (f.hdr.type) {
    case MESH_MSG_JOIN_ACK:
        if (n->join_pending && f.u.join_ack.status == MESH_JOIN_ACCEPTED) {
            n->join_pending = false;
            n->joined = true;
            n->join_attempt = 1;
            n->consecutive_fail = 0;
            n->timer_gen++; /* cancel the join timeout */
            node_kick(n);
        }
        break;
    case MESH_MSG_ACK:
        if (n->busy) {
            if (f.u.ack.status == MESH_ACK_BUSY) {
                n->truth.busy_acks++;
            }
            n->last_ack_rssi = rssi;
            const mesh_tx_action_t act =
                mesh_tx_on_ack(&n->tx, f.u.ack.acked_seq, f.u.ack.acked_attempt, f.u.ack.status, s_now);
            if (act != MESH_TX_ACTION_WAIT || f.u.ack.status == MESH_ACK_BUSY) {
                node_handle_action(n, act);
            }
        }
        break;
    case MESH_MSG_CONFIG: {
        const node_cfg_result_t r = node_config_apply(&n->cfg, f.u.config.key, f.u.config.value);
        node_send_ack(n, &f, r == NODE_CFG_INVALID ? MESH_ACK_INVALID : MESH_ACK_OK, rssi);
        if (r == NODE_CFG_APPLIED) {
            /* The firmware wakes the periodic tasks: restart both schedules now. */
            event_t s = ev_make(s_now + (uint64_t)n->cfg.report_interval_ms * 1000u, EV_NODE_SENSE, n->idx);
            s.gen = ++n->sense_gen;
            ev_push(s);
            event_t h = ev_make(s_now + (uint64_t)n->cfg.heartbeat_interval_ms * 1000u, EV_NODE_HB, n->idx);
            h.gen = ++n->hb_gen;
            ev_push(h);
        }
        break;
    }
    default:
        break;
    }
}

static void node_power(node_t *n, bool on)
{
    n->powered = on;
    n->timer_gen++;
    n->sense_gen++;
    n->hb_gen++;
    if (!on) {
        n->busy = false;
        n->joined = false;
        n->join_pending = false;
        n->q_len = 0;
        return;
    }
    /* Cold boot: RAM state lost, NVS (config, boot counter) kept. */
    n->boot_count++;
    n->power_on_us = s_now;
    n->seq = 0;
    n->q_head = n->q_len = 0;
    n->tx_ok = n->tx_retries = n->tx_failed = n->q_drops = 0;
    n->rtt_sum = n->rtt_cnt = n->rtt_max = n->last_rtt = 0;
    n->join_attempt = 1;
    n->next_join_us = s_now;
    sensor_sim_setup(&n->sim, O.seed * 2654435761u ^ n->cfg.node_id ^ (n->boot_count << 20), SENSOR_SIM_ALL);
    mesh_tx_init(&n->tx, &NODE_POLICY, O.seed ^ (0x1000u + (uint32_t)n->idx) ^ n->boot_count);
    /* Stagger boots by a few ms like real hardware. */
    event_t s = ev_make(s_now + (uint64_t)n->cfg.report_interval_ms * 1000u, EV_NODE_SENSE, n->idx);
    s.gen = n->sense_gen;
    ev_push(s);
    event_t h = ev_make(s_now + (uint64_t)n->cfg.heartbeat_interval_ms * 1000u, EV_NODE_HB, n->idx);
    h.gen = n->hb_gen;
    ev_push(h);
    node_kick(n);
}

/* ------------------------------------------------------------------------- */
/* Gateway command path (mirrors gw_tasks.c send_config)                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool active;
    uint16_t node_id;
    uint8_t key;
    uint32_t value;
    uint8_t mac[MESH_MAC_LEN];
    uint8_t buf[MESH_FRAME_MAX];
    size_t len;
    mesh_tx_t tx;
    uint32_t gen;
} gw_cmd_t;

static gw_cmd_t s_cmd;

static node_t *node_by_mac(const uint8_t mac[MESH_MAC_LEN])
{
    for (int i = 0; i < O.nodes; i++) {
        if (memcmp(s_nodes[i].mac, mac, MESH_MAC_LEN) == 0) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static void gw_radio_send(const uint8_t mac[MESH_MAC_LEN], const uint8_t *buf, size_t len, uint64_t at,
                          bool cmd_send_done)
{
    node_t *n = node_by_mac(mac);
    if (n == NULL) {
        return;
    }
    const uint32_t air = airtime_us(len);
    const bool lost = channel_lost(&n->ch);
    if (!lost) {
        event_t a = ev_make(at + air, EV_FRAME_TO_NODE, n->idx);
        a.len = (uint8_t)len;
        memcpy(a.data, buf, len);
        a.arg = (uint32_t)(int32_t)channel_rssi(&n->ch);
        ev_push(a);
    }
    if (cmd_send_done) {
        event_t d = ev_make(at + air + MAC_ACK_US, EV_GW_CMD_SEND_DONE, -1);
        d.ok = !lost && n->powered;
        d.gen = s_cmd.gen;
        ev_push(d);
    }
}

static void gw_cmd_finish(bool done)
{
    gw_out_t r;
    memset(&r, 0, sizeof(r));
    r.kind = GW_OUT_CONFIG;
    r.ts_ms = now_ms();
    r.node_id = s_cmd.node_id;
    memcpy(r.mac, s_cmd.mac, MESH_MAC_LEN);
    r.u.config.key = s_cmd.key;
    r.u.config.value = s_cmd.value;
    r.u.config.status = done ? s_cmd.tx.acked_status : 0xFF;
    r.u.config.attempts = s_cmd.tx.attempt;
    emit(&r);
    s_cmd.active = false;
}

static void gw_cmd_action(mesh_tx_action_t act)
{
    switch (act) {
    case MESH_TX_ACTION_SEND:
        mesh_frame_set_attempt(s_cmd.buf, s_cmd.len, s_cmd.tx.attempt);
        s_cmd.gen++;
        gw_radio_send(s_cmd.mac, s_cmd.buf, s_cmd.len, s_now, true);
        break;
    case MESH_TX_ACTION_DONE:
        gw_cmd_finish(true);
        break;
    case MESH_TX_ACTION_FAILED:
        gw_cmd_finish(false);
        break;
    default: {
        event_t e = ev_make(mesh_tx_deadline(&s_cmd.tx), EV_GW_CMD_TIMER, -1);
        e.gen = ++s_cmd.gen;
        ev_push(e);
        break;
    }
    }
}

static void gw_cmd_start(uint16_t node_id, uint8_t key, uint32_t value)
{
    if (s_cmd.active) {
        return;
    }
    uint16_t seq;
    if (!gw_core_build_config(&s_gw, node_id, key, value, now_ms(), s_cmd.buf, &s_cmd.len, &seq, s_cmd.mac)) {
        return;
    }
    const mesh_retry_policy_t pol = {.max_attempts = 4, .ack_timeout_us = 100000, .backoff_base_us = 50000,
                                     .backoff_max_us = 400000, .jitter_us = 20000};
    s_cmd.active = true;
    s_cmd.node_id = node_id;
    s_cmd.key = key;
    s_cmd.value = value;
    mesh_tx_init(&s_cmd.tx, &pol, O.seed ^ 0xC0FFEEu ^ seq);
    gw_cmd_action(mesh_tx_start(&s_cmd.tx, seq));
}

/* ------------------------------------------------------------------------- */
/* Gateway frame handling                                                    */
/* ------------------------------------------------------------------------- */

static void gw_on_frame(const event_t *e)
{
    node_t *n = &s_nodes[e->node];
    static gw_rx_result_t res;
    /* The gateway's RX task needs 0.3-0.8 ms to validate, account and answer. */
    const uint64_t proc_us = 300u + mesh_rand_below(&n->ch.rng, 500);
    gw_core_handle_frame(&s_gw, n->mac, e->data, e->len, (int8_t)(int32_t)e->arg, now_ms(), out_can_forward(),
                         &res);
    for (size_t i = 0; i < res.n_out; i++) {
        emit(&res.out[i]);
    }
    if (res.reply) {
        gw_radio_send(res.reply_mac, res.reply_buf, res.reply_len, s_now + proc_us, false);
    }
    if (res.ack_for_gateway && s_cmd.active && res.ack_from_node == s_cmd.node_id) {
        gw_cmd_action(mesh_tx_on_ack(&s_cmd.tx, res.ack.acked_seq, res.ack.acked_attempt, res.ack.status, s_now));
    }
}

static void gw_emit_stats(void)
{
    gw_out_t recs[MAX_NODES + 1];
    const size_t n = gw_core_link_records(&s_gw, now_ms(), recs, MAX_NODES);
    gw_out_t *st = &recs[n];
    memset(st, 0, sizeof(*st));
    st->kind = GW_OUT_GW_STATS;
    st->ts_ms = now_ms();
    gw_core_fill_stats(&s_gw, &st->u.stats);
    st->u.stats.uptime_s = now_ms() / 1000u;
    st->u.stats.out_queue_drops = s_out_drops;
    st->u.stats.out_queue_peak = s_out_peak;
    for (size_t i = 0; i <= n; i++) {
        emit(&recs[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* Setup, main loop, reports                                                 */
/* ------------------------------------------------------------------------- */

static void setup_nodes(void)
{
    for (int i = 0; i < O.nodes; i++) {
        node_t *n = &s_nodes[i];
        memset(n, 0, sizeof(*n));
        n->idx = i;
        const uint8_t mac[MESH_MAC_LEN] = {0x24, 0x0A, 0xC4, 0x5E, 0x00, (uint8_t)(0x10 + i)};
        memcpy(n->mac, mac, MESH_MAC_LEN);
        node_config_defaults(&n->cfg, (uint16_t)(i + 1), O.report_ms, O.hb_ms);
        /* Farther nodes: weaker signal, more background loss, burstier channel. */
        n->ch.rng = O.seed ^ (0xA5A5u + 977u * (uint32_t)i);
        n->ch.p_gb = 0.005 + 0.003 * i;
        n->ch.p_bg = 0.25;
        n->ch.loss_good = 0.005 + 0.004 * i;
        n->ch.loss_bad = 0.6;
        n->ch.rssi_base = -48 - 6 * i;
        n->join_rng = O.seed ^ (0x5151u + (uint32_t)i);
        /* Power up staggered by 0-49 ms. */
        event_t e = ev_make((uint64_t)(i * 7919 % 50) * 1000u + 10000u, EV_NODE_POWER, i);
        e.ok = true;
        ev_push(e);
    }
}

static void schedule_scenario(void)
{
    if (!O.scenario_events) {
        return;
    }
    const double d = O.duration_s;
    /* Durations below are fractions of the run so shorter CI runs keep the same shape. */
    /* 1. Power loss of node 3 (then cold boot + re-join). */
    if (O.nodes >= 3) {
        event_t off = ev_make((uint64_t)(d * 0.33 * 1e6), EV_NODE_POWER, 2);
        off.ok = false;
        ev_push(off);
        event_t on = ev_make((uint64_t)(d * 0.43 * 1e6), EV_NODE_POWER, 2);
        on.ok = true;
        ev_push(on);
    }
    /* 2. Interference burst on node 5: 60 % loss (arg = loss %). */
    if (O.nodes >= 5) {
        event_t b = ev_make((uint64_t)(d * 0.5 * 1e6), EV_NODE_INTERFERENCE, 4);
        b.arg = 60;
        ev_push(b);
        event_t c = ev_make((uint64_t)(d * 0.6 * 1e6), EV_NODE_INTERFERENCE, 4);
        c.arg = 0;
        ev_push(c);
    }
    /* 3. Remote reconfiguration of node 1 (faster reporting, then back). */
    event_t c1 = ev_make((uint64_t)(d * 0.66 * 1e6), EV_GW_CMD, -1);
    c1.arg = 500;
    ev_push(c1);
    event_t c2 = ev_make((uint64_t)(d * 0.83 * 1e6), EV_GW_CMD, -1);
    c2.arg = O.report_ms;
    ev_push(c2);
}

static void write_truth(void)
{
    FILE *f = fopen(O.truth_path, "w");
    if (f == NULL) {
        perror(O.truth_path);
        exit(2);
    }
    fprintf(f, "{\n  \"seed\": %" PRIu32 ",\n  \"nodes\": %d,\n  \"duration_s\": %.1f,\n  \"baud\": %" PRIu32
               ",\n  \"report_ms\": %" PRIu32 ",\n  \"hb_ms\": %" PRIu32 ",\n",
            O.seed, O.nodes, O.duration_s, O.baud, O.report_ms, O.hb_ms);
    fprintf(f, "  \"retry_policy\": {\"max_attempts\": %u, \"ack_timeout_us\": %" PRIu32
               ", \"backoff_base_us\": %" PRIu32 ", \"backoff_max_us\": %" PRIu32 ", \"jitter_us\": %" PRIu32 "},\n",
            NODE_POLICY.max_attempts, NODE_POLICY.ack_timeout_us, NODE_POLICY.backoff_base_us,
            NODE_POLICY.backoff_max_us, NODE_POLICY.jitter_us);
    fprintf(f, "  \"gateway\": {\"lines\": %" PRIu64 ", \"bytes\": %" PRIu64 ", \"out_queue_peak\": %" PRIu32
               ", \"out_queue_drops\": %" PRIu32 ", \"rx_frames\": %" PRIu32 ", \"duplicates\": %" PRIu32
               ", \"busy_acks\": %" PRIu32 "},\n",
            s_lines, s_bytes, s_out_peak, s_out_drops, s_gw.cnt.rx_frames, s_gw.cnt.duplicates, s_gw.cnt.busy_acks);
    fprintf(f, "  \"per_node\": [\n");
    for (int i = 0; i < O.nodes; i++) {
        const node_t *n = &s_nodes[i];
        const truth_t *t = &n->truth;
        fprintf(f,
                "    {\"node\": %u, \"mac\": \"%02x:%02x:%02x:%02x:%02x:%02x\", \"loss_good\": %.4f, "
                "\"loss_bad\": %.2f, \"p_gb\": %.4f, \"p_bg\": %.2f, \"rssi_base\": %d, \"boots\": %" PRIu32
                ", \"readings\": %" PRIu64 ", \"queue_drops\": %" PRIu64 ", \"data_sent\": %" PRIu64
                ", \"data_acked\": %" PRIu64 ", \"data_failed\": %" PRIu64 ", \"hb_sent\": %" PRIu64
                ", \"hb_acked\": %" PRIu64 ", \"hb_failed\": %" PRIu64 ", \"attempts\": %" PRIu64
                ", \"joins_sent\": %" PRIu64 ", \"rejoins\": %" PRIu64 ", \"busy_acks\": %" PRIu64 "}%s\n",
                n->cfg.node_id, n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5], n->ch.loss_good,
                n->ch.loss_bad, n->ch.p_gb, n->ch.p_bg, n->ch.rssi_base, n->boot_count, t->readings, t->queue_drops,
                t->data_sent, t->data_acked, t->data_failed, t->hb_sent, t->hb_acked, t->hb_failed, t->attempts,
                t->joins_sent, t->rejoins, t->busy_acks, i + 1 < O.nodes ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--nodes N] [--duration S] [--seed N] [--baud N] [--report-ms N] [--hb-ms N]\n"
            "          [--no-events] --out gateway.jsonl --truth truth.json\n",
            argv0);
    exit(2);
}

static void parse_args(int argc, char **argv)
{
    O = (opts_t){.nodes = 6, .duration_s = 600, .seed = 42, .baud = 921600, .report_ms = 1000, .hb_ms = 5000,
                 .scenario_events = true};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--no-events") == 0) {
            O.scenario_events = false;
            continue;
        }
        if (v == NULL) {
            usage(argv[0]);
        }
        if (strcmp(a, "--nodes") == 0) {
            O.nodes = atoi(v);
        } else if (strcmp(a, "--duration") == 0) {
            O.duration_s = atof(v);
        } else if (strcmp(a, "--seed") == 0) {
            O.seed = (uint32_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--baud") == 0) {
            O.baud = (uint32_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--report-ms") == 0) {
            O.report_ms = (uint32_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--hb-ms") == 0) {
            O.hb_ms = (uint32_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--out") == 0) {
            O.out_path = v;
        } else if (strcmp(a, "--truth") == 0) {
            O.truth_path = v;
        } else {
            usage(argv[0]);
        }
        i++;
    }
    if (O.nodes < 1 || O.nodes > MAX_NODES || O.duration_s <= 0 || O.baud < 9600 || !O.out_path ||
        !O.truth_path || O.report_ms < NODE_REPORT_MS_MIN || O.hb_ms < NODE_HB_MS_MIN) {
        usage(argv[0]);
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    s_out = fopen(O.out_path, "w");
    if (s_out == NULL) {
        perror(O.out_path);
        return 2;
    }
    const gw_liveness_policy_t pol = {.suspect_misses = 2, .offline_misses = 4, .grace_ms = 1000};
    gw_core_init(&s_gw, MAX_NODES, &pol, 1);
    gw_fsm_init(&s_fsm);

    /* Gateway boot sequence, same records as the firmware. */
    emit_gw_state(GW_EV_BOOT_DONE);
    gw_out_t boot;
    memset(&boot, 0, sizeof(boot));
    boot.kind = GW_OUT_BOOT;
    const uint8_t gw_mac[MESH_MAC_LEN] = {0x24, 0x0A, 0xC4, 0x5E, 0x00, 0x01};
    memcpy(boot.mac, gw_mac, MESH_MAC_LEN);
    boot.u.boot.fw_version = mesh_fw_version(1, 0, 0);
    boot.u.boot.channel = 1;
    boot.u.boot.capacity = MAX_NODES;
    emit(&boot);
    char info[96];
    snprintf(info, sizeof(info), "host simulator: seed=%" PRIu32 " nodes=%d duration=%.0fs baud=%" PRIu32, O.seed,
             O.nodes, O.duration_s, O.baud);
    emit_info(info);
    emit_gw_state(GW_EV_REGISTRY_LOADED);
    emit_gw_state(GW_EV_RADIO_READY);

    setup_nodes();
    schedule_scenario();
    ev_push(ev_make(GW_TICK_US, EV_GW_TICK, -1));
    ev_push(ev_make(30000000u, EV_GW_STATS, -1));

    const uint64_t end = (uint64_t)(O.duration_s * 1e6);
    while (s_heap_len > 0) {
        event_t e = ev_pop();
        if (e.t > end) {
            break;
        }
        s_now = e.t;
        node_t *n = (e.node >= 0) ? &s_nodes[e.node] : NULL;
        switch (e.kind) {
        case EV_NODE_POWER:
            node_power(n, e.ok);
            break;
        case EV_NODE_INTERFERENCE:
            n->ch.forced_loss = e.arg / 100.0;
            break;
        case EV_NODE_SENSE:
            if (n->powered && e.gen == n->sense_gen) {
                q_item_t it = {.is_hb = false};
                sensor_reading_clear(&it.reading);
                const sensor_driver_t d = sensor_sim_driver(&n->sim);
                (void)d.read(d.ctx, (uint32_t)((s_now - n->power_on_us) / 1000u), &it.reading);
                n->truth.readings++;
                node_enqueue(n, &it, false);
                event_t nx = ev_make(s_now + (uint64_t)n->cfg.report_interval_ms * 1000u, EV_NODE_SENSE, n->idx);
                nx.gen = n->sense_gen;
                ev_push(nx);
            }
            break;
        case EV_NODE_HB:
            if (n->powered && e.gen == n->hb_gen) {
                const q_item_t it = {.is_hb = true};
                node_enqueue(n, &it, true);
                event_t nx = ev_make(s_now + (uint64_t)n->cfg.heartbeat_interval_ms * 1000u, EV_NODE_HB, n->idx);
                nx.gen = n->hb_gen;
                ev_push(nx);
            }
            break;
        case EV_NODE_SEND_DONE:
            if (n->powered && n->busy && e.gen == n->timer_gen) {
                mesh_tx_sent(&n->tx, s_now);
                node_handle_action(n, e.ok ? MESH_TX_ACTION_WAIT : mesh_tx_on_radio_fail(&n->tx, s_now));
            }
            break;
        case EV_NODE_TIMER:
            if (!n->powered || e.gen != n->timer_gen) {
                break;
            }
            if (n->join_pending) {
                n->join_pending = false;
                if (n->join_attempt < 16) {
                    n->join_attempt++;
                }
                n->next_join_us = s_now + mesh_retry_backoff_us(&JOIN_POLICY, n->join_attempt, &n->join_rng);
                event_t k = ev_make(n->next_join_us, EV_NODE_TIMER, n->idx);
                k.gen = ++n->timer_gen;
                k.arg = 1; /* join backoff expired */
                ev_push(k);
            } else if (e.arg == 1 && !n->joined && !n->busy) {
                node_kick(n);
            } else if (n->busy) {
                node_handle_action(n, mesh_tx_poll(&n->tx, s_now));
            }
            break;
        case EV_FRAME_TO_GW:
            if (s_nodes[e.node].powered) {
                gw_on_frame(&e);
            }
            break;
        case EV_FRAME_TO_NODE:
            node_on_frame(n, e.data, e.len, (int8_t)(int32_t)e.arg);
            break;
        case EV_GW_TICK: {
            gw_out_t recs[MAX_NODES];
            const size_t k = gw_core_tick(&s_gw, now_ms(), recs, MAX_NODES);
            for (size_t i = 0; i < k; i++) {
                emit(&recs[i]);
            }
            out_expire();
            const uint32_t fill = 100u * (uint32_t)s_out_len / GW_OUT_CAP;
            if (fill >= 75 && s_fsm.state == GW_ST_RUNNING) {
                emit_gw_state(GW_EV_QUEUE_PRESSURE);
            } else if (fill <= 25 && s_fsm.state == GW_ST_DEGRADED) {
                emit_gw_state(GW_EV_QUEUE_RELIEVED);
            }
            ev_push(ev_make(s_now + GW_TICK_US, EV_GW_TICK, -1));
            break;
        }
        case EV_GW_STATS:
            gw_emit_stats();
            ev_push(ev_make(s_now + 30000000u, EV_GW_STATS, -1));
            break;
        case EV_GW_CMD:
            gw_cmd_start(1, MESH_CFG_REPORT_INTERVAL_MS, e.arg);
            break;
        case EV_GW_CMD_TIMER:
            if (s_cmd.active && e.gen == s_cmd.gen) {
                gw_cmd_action(mesh_tx_poll(&s_cmd.tx, s_now));
            }
            break;
        case EV_GW_CMD_SEND_DONE:
            if (s_cmd.active && e.gen == s_cmd.gen) {
                mesh_tx_sent(&s_cmd.tx, s_now);
                gw_cmd_action(e.ok ? MESH_TX_ACTION_WAIT : mesh_tx_on_radio_fail(&s_cmd.tx, s_now));
            }
            break;
        default:
            break;
        }
    }
    s_now = end;
    gw_emit_stats(); /* final link summaries */
    fclose(s_out);
    write_truth();
    free(s_heap);
    fprintf(stderr, "mesh_sim: %" PRIu64 " lines, %d nodes, %.0f s simulated, seed %" PRIu32 "\n", s_lines,
            O.nodes, O.duration_s, O.seed);
    return 0;
}
