/*
 * SPDX-License-Identifier: MIT
 */
#include "gw_registry.h"

#include <string.h>

#include "mesh_crc.h"

#define BLOB_MAGIC 0x47575247u /* 'GWRG' */
#define RECORD_SIZE 26u        /* mac 6 + id 2 + backend 1 + pad 1 + fw 4 + boot 4 + report 4 + hb 4 */
#define HEADER_SIZE 8u         /* magic 4 + version 1 + count 1 + crc 2 */

void gw_registry_init(gw_registry_t *reg, uint8_t capacity, const gw_liveness_policy_t *policy)
{
    memset(reg, 0, sizeof(*reg));
    reg->capacity = (capacity == 0 || capacity > GW_REGISTRY_CAPACITY) ? GW_REGISTRY_CAPACITY : capacity;
    if (policy != NULL) {
        reg->policy = *policy;
    } else {
        reg->policy.suspect_misses = 2;
        reg->policy.offline_misses = 4;
        reg->policy.grace_ms = 1000;
    }
    if (reg->policy.suspect_misses == 0) {
        reg->policy.suspect_misses = 1;
    }
    if (reg->policy.offline_misses <= reg->policy.suspect_misses) {
        reg->policy.offline_misses = (uint8_t)(reg->policy.suspect_misses + 1);
    }
}

gw_node_t *gw_registry_find_mac(gw_registry_t *reg, const uint8_t mac[MESH_MAC_LEN])
{
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
        if (reg->nodes[i].used && memcmp(reg->nodes[i].mac, mac, MESH_MAC_LEN) == 0) {
            return &reg->nodes[i];
        }
    }
    return NULL;
}

gw_node_t *gw_registry_find_id(gw_registry_t *reg, uint16_t node_id)
{
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
        if (reg->nodes[i].used && reg->nodes[i].node_id == node_id) {
            return &reg->nodes[i];
        }
    }
    return NULL;
}

size_t gw_registry_count(const gw_registry_t *reg)
{
    size_t n = 0;
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
        n += reg->nodes[i].used ? 1u : 0u;
    }
    return n;
}

size_t gw_registry_count_state(const gw_registry_t *reg, gw_node_state_t state)
{
    size_t n = 0;
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
        n += (reg->nodes[i].used && reg->nodes[i].state == state) ? 1u : 0u;
    }
    return n;
}

gw_join_result_t gw_registry_join(gw_registry_t *reg, const uint8_t mac[MESH_MAC_LEN], uint16_t node_id,
                                  const mesh_join_payload_t *join, uint64_t now_ms, gw_node_t **out)
{
    gw_node_t *owner = gw_registry_find_id(reg, node_id);
    gw_node_t *n = gw_registry_find_mac(reg, mac);
    if (owner != NULL && owner != n) {
        if (out) {
            *out = NULL;
        }
        return GW_JOIN_ID_CONFLICT;
    }
    gw_join_result_t res = GW_JOIN_OK_KNOWN;
    bool reset_seq = (n == NULL) || (n->boot_count != join->boot_count);
    if (n == NULL) {
        if (gw_registry_count(reg) >= reg->capacity) {
            if (out) {
                *out = NULL;
            }
            return GW_JOIN_FULL;
        }
        for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
            if (!reg->nodes[i].used) {
                n = &reg->nodes[i];
                break;
            }
        }
        memset(n, 0, sizeof(*n));
        n->used = true;
        memcpy(n->mac, mac, MESH_MAC_LEN);
        res = GW_JOIN_OK_NEW;
    }
    /* A MAC may legitimately change its id (NVS erased / reconfigured). */
    if (n->node_id != node_id || n->report_interval_ms != join->report_interval_ms ||
        n->heartbeat_interval_ms != join->heartbeat_interval_ms || n->backend != join->backend ||
        n->fw_version != join->fw_version) {
        if (res == GW_JOIN_OK_KNOWN) {
            res = GW_JOIN_OK_NEW; /* identity changed: must be persisted again */
        }
    }
    n->node_id = node_id;
    n->backend = join->backend;
    n->fw_version = join->fw_version;
    n->boot_count = join->boot_count;
    n->report_interval_ms = join->report_interval_ms;
    n->heartbeat_interval_ms = join->heartbeat_interval_ms;
    n->state = GW_NODE_ONLINE;
    n->last_seen_ms = now_ms;
    if (reset_seq) {
        /* New node or reboot: its sequence counter restarted. A re-join after
         * a link loss keeps the tracker, so frames lost during the outage
         * still show up as gaps. */
        mesh_seq_tracker_reset(&n->seq);
    }
    if (out) {
        *out = n;
    }
    return res;
}

mesh_seq_result_t gw_registry_on_frame(gw_registry_t *reg, gw_node_t *node, uint16_t seq, int8_t rssi,
                                       uint64_t now_ms, uint32_t *gap, gw_transition_t *transition,
                                       bool *transitioned)
{
    (void)reg;
    *transitioned = false;
    if (node->state != GW_NODE_ONLINE) {
        transition->node_id = node->node_id;
        transition->from = node->state;
        transition->to = GW_NODE_ONLINE;
        transition->silent_ms = (uint32_t)(now_ms - node->last_seen_ms);
        *transitioned = true;
        node->state = GW_NODE_ONLINE;
    }
    node->last_seen_ms = now_ms;
    node->last_rssi = rssi;
    return mesh_seq_tracker_update(&node->seq, seq, gap);
}

static uint32_t hb_or_default(const gw_node_t *node)
{
    return node->heartbeat_interval_ms ? node->heartbeat_interval_ms : 5000u;
}

uint32_t gw_registry_suspect_ms(const gw_registry_t *reg, const gw_node_t *node)
{
    return hb_or_default(node) * reg->policy.suspect_misses + reg->policy.grace_ms;
}

uint32_t gw_registry_offline_ms(const gw_registry_t *reg, const gw_node_t *node)
{
    return hb_or_default(node) * reg->policy.offline_misses + reg->policy.grace_ms;
}

size_t gw_registry_tick(gw_registry_t *reg, uint64_t now_ms, gw_transition_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY && n < max; i++) {
        gw_node_t *node = &reg->nodes[i];
        if (!node->used || node->state == GW_NODE_OFFLINE) {
            continue;
        }
        const uint64_t silent = (now_ms > node->last_seen_ms) ? now_ms - node->last_seen_ms : 0;
        gw_node_state_t next = node->state;
        if (silent > gw_registry_offline_ms(reg, node)) {
            next = GW_NODE_OFFLINE;
        } else if (silent > gw_registry_suspect_ms(reg, node)) {
            next = GW_NODE_SUSPECT;
        }
        if (next != node->state) {
            out[n].node_id = node->node_id;
            out[n].from = node->state;
            out[n].to = next;
            out[n].silent_ms = (uint32_t)silent;
            n++;
            node->state = next;
        }
    }
    return n;
}

bool gw_registry_forget(gw_registry_t *reg, uint16_t node_id, uint8_t mac_out[MESH_MAC_LEN])
{
    gw_node_t *n = gw_registry_find_id(reg, node_id);
    if (n == NULL) {
        return false;
    }
    if (mac_out != NULL) {
        memcpy(mac_out, n->mac, MESH_MAC_LEN);
    }
    memset(n, 0, sizeof(*n));
    return true;
}

/* ---------------------------- persistence -------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t gw_registry_blob_size(const gw_registry_t *reg)
{
    return HEADER_SIZE + RECORD_SIZE * gw_registry_count(reg);
}

bool gw_registry_serialize(const gw_registry_t *reg, uint8_t *buf, size_t cap, size_t *len)
{
    const size_t need = gw_registry_blob_size(reg);
    if (buf == NULL || cap < need) {
        return false;
    }
    uint8_t *p = buf + HEADER_SIZE;
    uint8_t count = 0;
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
        const gw_node_t *n = &reg->nodes[i];
        if (!n->used) {
            continue;
        }
        memcpy(p, n->mac, MESH_MAC_LEN);
        p[6] = (uint8_t)n->node_id;
        p[7] = (uint8_t)(n->node_id >> 8);
        p[8] = n->backend;
        p[9] = 0;
        put32(p + 10, n->fw_version);
        put32(p + 14, n->boot_count);
        put32(p + 18, n->report_interval_ms);
        put32(p + 22, n->heartbeat_interval_ms);
        p += RECORD_SIZE;
        count++;
    }
    put32(buf, BLOB_MAGIC);
    buf[4] = GW_REGISTRY_BLOB_VERSION;
    buf[5] = count;
    const uint16_t crc = mesh_crc16(buf + HEADER_SIZE, (size_t)count * RECORD_SIZE);
    buf[6] = (uint8_t)crc;
    buf[7] = (uint8_t)(crc >> 8);
    *len = need;
    return true;
}

int gw_registry_deserialize(gw_registry_t *reg, const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < HEADER_SIZE || get32(buf) != BLOB_MAGIC || buf[4] != GW_REGISTRY_BLOB_VERSION) {
        return -1;
    }
    const uint8_t count = buf[5];
    if (count > GW_REGISTRY_CAPACITY || len != HEADER_SIZE + (size_t)count * RECORD_SIZE) {
        return -1;
    }
    const uint16_t crc = (uint16_t)(buf[6] | (buf[7] << 8));
    if (mesh_crc16(buf + HEADER_SIZE, (size_t)count * RECORD_SIZE) != crc) {
        return -1;
    }
    for (size_t i = 0; i < GW_REGISTRY_CAPACITY; i++) {
        memset(&reg->nodes[i], 0, sizeof(reg->nodes[i]));
    }
    const uint8_t *p = buf + HEADER_SIZE;
    for (uint8_t i = 0; i < count; i++, p += RECORD_SIZE) {
        gw_node_t *n = &reg->nodes[i];
        n->used = true;
        memcpy(n->mac, p, MESH_MAC_LEN);
        n->node_id = (uint16_t)(p[6] | (p[7] << 8));
        n->backend = p[8];
        n->fw_version = get32(p + 10);
        n->boot_count = get32(p + 14);
        n->report_interval_ms = get32(p + 18);
        n->heartbeat_interval_ms = get32(p + 22);
        n->state = GW_NODE_OFFLINE; /* known, but not heard since boot */
    }
    return count;
}

const char *gw_node_state_str(gw_node_state_t s)
{
    switch (s) {
    case GW_NODE_OFFLINE: return "offline";
    case GW_NODE_ONLINE: return "online";
    case GW_NODE_SUSPECT: return "suspect";
    default: return "?";
    }
}
