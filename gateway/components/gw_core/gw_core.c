/*
 * SPDX-License-Identifier: MIT
 */
#include "gw_core.h"

#include <string.h>

void gw_core_init(gw_core_t *core, uint8_t capacity, const gw_liveness_policy_t *policy, uint8_t wifi_channel)
{
    memset(core, 0, sizeof(*core));
    gw_registry_init(&core->reg, capacity, policy);
    core->wifi_channel = wifi_channel;
}

static gw_out_t *push_out(gw_rx_result_t *res, gw_out_kind_t kind, uint64_t now_ms,
                          const uint8_t mac[MESH_MAC_LEN], uint16_t node_id)
{
    if (res->n_out >= GW_RX_MAX_OUT) {
        return NULL;
    }
    gw_out_t *o = &res->out[res->n_out++];
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    o->ts_ms = now_ms;
    o->node_id = node_id;
    memcpy(o->mac, mac, MESH_MAC_LEN);
    return o;
}

static void build_ack(gw_core_t *core, gw_rx_result_t *res, const uint8_t mac[MESH_MAC_LEN],
                      const mesh_header_t *acked, uint8_t status, int8_t rssi, uint64_t now_ms)
{
    mesh_frame_t f;
    mesh_frame_init(&f, MESH_MSG_ACK, MESH_GATEWAY_NODE_ID, core->tx_seq++, (uint32_t)now_ms, 0);
    f.u.ack.acked_seq = acked->seq;
    f.u.ack.acked_type = acked->type;
    f.u.ack.acked_attempt = acked->attempt;
    f.u.ack.status = status;
    f.u.ack.rssi = rssi;
    if (mesh_encode(&f, res->reply_buf, sizeof(res->reply_buf), &res->reply_len) == MESH_OK) {
        res->reply = true;
        memcpy(res->reply_mac, mac, MESH_MAC_LEN);
        core->cnt.acks_sent++;
        if (status == MESH_ACK_BUSY) {
            core->cnt.busy_acks++;
        }
    }
}

static void rx_error(gw_core_t *core, gw_rx_result_t *res, const uint8_t mac[MESH_MAC_LEN], int err,
                     size_t len, uint64_t now_ms)
{
    if (err == GW_RXERR_UNKNOWN_NODE) {
        core->cnt.unknown_node++; /* not a corrupt frame: counted separately */
    } else {
        core->cnt.rx_errors++;
    }
    gw_node_t *node = gw_registry_find_mac(&core->reg, mac);
    if (node != NULL) {
        node->rx_errors++;
    }
    gw_out_t *o = push_out(res, GW_OUT_RX_ERROR, now_ms, mac, node ? node->node_id : MESH_BROADCAST_NODE_ID);
    if (o != NULL) {
        o->u.rx_error.err = (int16_t)err;
        o->u.rx_error.len = (uint8_t)(len > 255 ? 255 : len);
    }
}

static void handle_join(gw_core_t *core, const uint8_t mac[MESH_MAC_LEN], const mesh_frame_t *f, int8_t rssi,
                        uint64_t now_ms, gw_rx_result_t *res)
{
    core->cnt.joins++;
    /* Remember the liveness state before the JOIN resets it to ONLINE, so a
     * node re-joining after a reboot or link loss still reports recovery. */
    const gw_node_t *prev = gw_registry_find_mac(&core->reg, mac);
    const gw_node_state_t prev_state = prev ? prev->state : GW_NODE_ONLINE;
    const uint64_t prev_seen = prev ? prev->last_seen_ms : now_ms;
    gw_node_t *node = NULL;
    gw_join_result_t jr;
    if (f->hdr.node_id == MESH_GATEWAY_NODE_ID || f->hdr.node_id == MESH_BROADCAST_NODE_ID) {
        jr = GW_JOIN_ID_CONFLICT;
    } else {
        jr = gw_registry_join(&core->reg, mac, f->hdr.node_id, &f->u.join, now_ms, &node);
    }
    uint8_t status;
    switch (jr) {
    case GW_JOIN_OK_NEW:
    case GW_JOIN_OK_KNOWN:
        status = MESH_JOIN_ACCEPTED;
        break;
    case GW_JOIN_FULL:
        status = MESH_JOIN_REJECTED_FULL;
        break;
    case GW_JOIN_ID_CONFLICT:
    default:
        status = MESH_JOIN_REJECTED_ID_CONFLICT;
        break;
    }
    uint32_t gap = 0;
    mesh_seq_result_t sr = MESH_SEQ_NEW;
    if (node != NULL) {
        bool transitioned;
        gw_transition_t tr;
        sr = gw_registry_on_frame(&core->reg, node, f->hdr.seq, rssi, now_ms, &gap, &tr, &transitioned);
        res->add_peer = true;
        res->persist = (jr == GW_JOIN_OK_NEW);
        if (prev_state != GW_NODE_ONLINE) {
            gw_out_t *o = push_out(res, GW_OUT_NODE_STATE, now_ms, mac, node->node_id);
            if (o != NULL) {
                o->u.node_state.node_id = node->node_id;
                o->u.node_state.from = prev_state;
                o->u.node_state.to = GW_NODE_ONLINE;
                o->u.node_state.silent_ms = (uint32_t)(now_ms - prev_seen);
            }
        }
    } else {
        res->reply_needs_temp_peer = true;
    }

    mesh_frame_t ack;
    mesh_frame_init(&ack, MESH_MSG_JOIN_ACK, MESH_GATEWAY_NODE_ID, core->tx_seq++, (uint32_t)now_ms, 0);
    ack.u.join_ack.status = status;
    ack.u.join_ack.wifi_channel = core->wifi_channel;
    ack.u.join_ack.offline_timeout_ms = node ? gw_registry_offline_ms(&core->reg, node) : 0;
    if (mesh_encode(&ack, res->reply_buf, sizeof(res->reply_buf), &res->reply_len) == MESH_OK) {
        res->reply = true;
        memcpy(res->reply_mac, mac, MESH_MAC_LEN);
    }

    gw_out_t *o = push_out(res, GW_OUT_JOIN, now_ms, mac, f->hdr.node_id);
    if (o != NULL) {
        o->seq = f->hdr.seq;
        o->attempt = f->hdr.attempt;
        o->rssi = rssi;
        o->node_uptime_ms = f->hdr.uptime_ms;
        o->gap = gap;
        o->seq_result = (uint8_t)sr;
        o->u.join.p = f->u.join;
        o->u.join.status = status;
        o->u.join.is_new = (jr == GW_JOIN_OK_NEW);
    }
}

static void handle_report(gw_core_t *core, const uint8_t mac[MESH_MAC_LEN], const mesh_frame_t *f, int8_t rssi,
                          uint64_t now_ms, bool can_forward, gw_rx_result_t *res)
{
    const bool wants_ack = (f->hdr.flags & MESH_FLAG_ACK_REQ) != 0;
    gw_node_t *node = gw_registry_find_mac(&core->reg, mac);
    if (node == NULL || node->node_id != f->hdr.node_id) {
        /* Not joined (e.g. registry entry forgotten): ask the node to re-join. */
        if (wants_ack) {
            build_ack(core, res, mac, &f->hdr, MESH_ACK_REJECTED, rssi, now_ms);
            res->reply_needs_temp_peer = (node == NULL);
        }
        rx_error(core, res, mac, GW_RXERR_UNKNOWN_NODE, 0, now_ms);
        return;
    }
    if (!can_forward) {
        if (wants_ack) {
            build_ack(core, res, mac, &f->hdr, MESH_ACK_BUSY, rssi, now_ms);
        }
        return; /* not accounted: the node will retransmit the same seq */
    }

    uint32_t gap = 0;
    bool transitioned = false;
    gw_transition_t tr;
    const mesh_seq_result_t sr =
        gw_registry_on_frame(&core->reg, node, f->hdr.seq, rssi, now_ms, &gap, &tr, &transitioned);
    if (transitioned) {
        gw_out_t *o = push_out(res, GW_OUT_NODE_STATE, now_ms, mac, node->node_id);
        if (o != NULL) {
            o->u.node_state = tr;
        }
    }

    gw_out_t *o;
    if (sr == MESH_SEQ_DUPLICATE) {
        core->cnt.duplicates++;
        o = push_out(res, GW_OUT_DUP, now_ms, mac, node->node_id);
    } else if (f->hdr.type == MESH_MSG_DATA) {
        node->data_frames++;
        o = push_out(res, GW_OUT_DATA, now_ms, mac, node->node_id);
        if (o != NULL) {
            o->u.data = f->u.data;
        }
    } else {
        node->heartbeats++;
        o = push_out(res, GW_OUT_HEARTBEAT, now_ms, mac, node->node_id);
        if (o != NULL) {
            o->u.hb = f->u.hb;
        }
    }
    if (o != NULL) {
        o->seq = f->hdr.seq;
        o->attempt = f->hdr.attempt;
        o->rssi = rssi;
        o->seq_result = (uint8_t)sr;
        o->gap = gap;
        o->node_uptime_ms = f->hdr.uptime_ms;
    }
    if (wants_ack) {
        build_ack(core, res, mac, &f->hdr, sr == MESH_SEQ_DUPLICATE ? MESH_ACK_DUPLICATE : MESH_ACK_OK, rssi,
                  now_ms);
    }
}

void gw_core_handle_frame(gw_core_t *core, const uint8_t mac[MESH_MAC_LEN], const uint8_t *data, size_t len,
                          int8_t rssi, uint64_t now_ms, bool can_forward, gw_rx_result_t *res)
{
    res->reply = false;
    res->reply_needs_temp_peer = false;
    res->add_peer = false;
    res->persist = false;
    res->ack_for_gateway = false;
    res->n_out = 0;
    core->cnt.rx_frames++;

    mesh_frame_t f;
    const mesh_err_t err = mesh_decode(data, len, &f);
    if (err != MESH_OK) {
        rx_error(core, res, mac, err, len, now_ms);
        return;
    }
    switch ((mesh_msg_type_t)f.hdr.type) {
    case MESH_MSG_JOIN:
        handle_join(core, mac, &f, rssi, now_ms, res);
        break;
    case MESH_MSG_DATA:
    case MESH_MSG_HEARTBEAT:
        handle_report(core, mac, &f, rssi, now_ms, can_forward, res);
        break;
    case MESH_MSG_ACK: {
        gw_node_t *node = gw_registry_find_mac(&core->reg, mac);
        if (node == NULL) {
            rx_error(core, res, mac, GW_RXERR_UNKNOWN_NODE, len, now_ms);
            return;
        }
        uint32_t gap;
        bool transitioned;
        gw_transition_t tr;
        (void)gw_registry_on_frame(&core->reg, node, f.hdr.seq, rssi, now_ms, &gap, &tr, &transitioned);
        if (transitioned) {
            gw_out_t *o = push_out(res, GW_OUT_NODE_STATE, now_ms, mac, node->node_id);
            if (o != NULL) {
                o->u.node_state = tr;
            }
        }
        res->ack_for_gateway = true;
        res->ack = f.u.ack;
        res->ack_from_node = node->node_id;
        break;
    }
    case MESH_MSG_JOIN_ACK:
    case MESH_MSG_CONFIG:
    default:
        rx_error(core, res, mac, GW_RXERR_BAD_DIRECTION, len, now_ms);
        break;
    }
}

size_t gw_core_tick(gw_core_t *core, uint64_t now_ms, gw_out_t *out, size_t max)
{
    gw_transition_t tr[GW_REGISTRY_CAPACITY];
    const size_t n = gw_registry_tick(&core->reg, now_ms, tr, max < GW_REGISTRY_CAPACITY ? max : GW_REGISTRY_CAPACITY);
    for (size_t i = 0; i < n; i++) {
        gw_node_t *node = gw_registry_find_id(&core->reg, tr[i].node_id);
        memset(&out[i], 0, sizeof(out[i]));
        out[i].kind = GW_OUT_NODE_STATE;
        out[i].ts_ms = now_ms;
        out[i].node_id = tr[i].node_id;
        if (node != NULL) {
            memcpy(out[i].mac, node->mac, MESH_MAC_LEN);
        }
        out[i].u.node_state = tr[i];
    }
    return n;
}

size_t gw_core_link_records(gw_core_t *core, uint64_t now_ms, gw_out_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY && n < max; i++) {
        const gw_node_t *node = &core->reg.nodes[i];
        if (!node->used) {
            continue;
        }
        gw_out_t *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->kind = GW_OUT_LINK;
        o->ts_ms = now_ms;
        o->node_id = node->node_id;
        memcpy(o->mac, node->mac, MESH_MAC_LEN);
        o->u.link.seq = node->seq;
        o->u.link.state = (uint8_t)node->state;
        o->u.link.rssi = node->last_rssi;
        o->u.link.rx_errors = node->rx_errors;
    }
    return n;
}

bool gw_core_build_config(gw_core_t *core, uint16_t node_id, uint8_t key, uint32_t value, uint64_t now_ms,
                          uint8_t *buf, size_t *len, uint16_t *seq, uint8_t mac_out[MESH_MAC_LEN])
{
    gw_node_t *node = gw_registry_find_id(&core->reg, node_id);
    if (node == NULL) {
        return false;
    }
    mesh_frame_t f;
    *seq = core->tx_seq++;
    mesh_frame_init(&f, MESH_MSG_CONFIG, MESH_GATEWAY_NODE_ID, *seq, (uint32_t)now_ms, MESH_FLAG_ACK_REQ);
    f.u.config.key = key;
    f.u.config.value = value;
    memcpy(mac_out, node->mac, MESH_MAC_LEN);
    return mesh_encode(&f, buf, MESH_FRAME_MAX, len) == MESH_OK;
}

void gw_core_fill_stats(const gw_core_t *core, gw_stats_t *s)
{
    s->rx_frames = core->cnt.rx_frames;
    s->rx_errors = core->cnt.rx_errors;
    s->acks_sent = core->cnt.acks_sent;
    s->busy_acks = core->cnt.busy_acks;
    s->unknown_node = core->cnt.unknown_node;
    s->duplicates = core->cnt.duplicates;
    s->nodes_total = (uint8_t)gw_registry_count(&core->reg);
    s->nodes_online = (uint8_t)gw_registry_count_state(&core->reg, GW_NODE_ONLINE);
    s->nodes_suspect = (uint8_t)gw_registry_count_state(&core->reg, GW_NODE_SUSPECT);
    s->nodes_offline = (uint8_t)gw_registry_count_state(&core->reg, GW_NODE_OFFLINE);
}
