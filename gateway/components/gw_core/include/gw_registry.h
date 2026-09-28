/*
 * SPDX-License-Identifier: MIT
 *
 * gw_registry.h - Node registry of the gateway (pure C, host-testable).
 *
 * Identity (MAC, id, announced intervals) is persisted in NVS so peers can be
 * re-added after a gateway reboot; link statistics are volatile.
 *
 * Per-node liveness state machine (driven by gw_registry_tick):
 *
 *   OFFLINE --any valid frame--> ONLINE --silence > suspect_ms--> SUSPECT
 *      ^                           ^                                  |
 *      |                           +------------any frame-------------+
 *      +--------------------silence > offline_ms----------------------+
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_protocol.h"
#include "mesh_seq.h"
#include "mesh_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_REGISTRY_CAPACITY 20 /* ESP-NOW peer limit; encrypted peers are limited to 17 */
#define GW_REGISTRY_BLOB_VERSION 1u

typedef enum {
    GW_NODE_OFFLINE = 0,
    GW_NODE_ONLINE = 1,
    GW_NODE_SUSPECT = 2,
} gw_node_state_t;

typedef struct {
    uint8_t suspect_misses; /**< Missed heartbeats before SUSPECT (e.g. 2). */
    uint8_t offline_misses; /**< Missed heartbeats before OFFLINE (e.g. 4). */
    uint32_t grace_ms;      /**< Added to both thresholds to absorb jitter/retries. */
} gw_liveness_policy_t;

typedef struct {
    bool used;
    uint8_t mac[MESH_MAC_LEN];
    uint16_t node_id;
    uint8_t backend;
    uint32_t fw_version;
    uint32_t boot_count;
    uint32_t report_interval_ms;
    uint32_t heartbeat_interval_ms;
    /* volatile state */
    gw_node_state_t state;
    uint64_t last_seen_ms;
    int8_t last_rssi;
    mesh_seq_tracker_t seq;
    uint32_t data_frames;
    uint32_t heartbeats;
    uint32_t rx_errors; /**< Frames from this MAC that failed validation. */
} gw_node_t;

typedef struct {
    gw_node_t nodes[GW_REGISTRY_CAPACITY];
    uint8_t capacity; /**< Runtime limit <= GW_REGISTRY_CAPACITY (Kconfig). */
    gw_liveness_policy_t policy;
} gw_registry_t;

typedef enum {
    GW_JOIN_OK_NEW = 0,     /**< New node registered: persist + add peer. */
    GW_JOIN_OK_KNOWN = 1,   /**< Known node re-joined (reboot / link loss). */
    GW_JOIN_FULL = 2,
    GW_JOIN_ID_CONFLICT = 3, /**< Another MAC already owns this node id. */
} gw_join_result_t;

typedef struct {
    uint16_t node_id;
    gw_node_state_t from;
    gw_node_state_t to;
    uint32_t silent_ms;
} gw_transition_t;

void gw_registry_init(gw_registry_t *reg, uint8_t capacity, const gw_liveness_policy_t *policy);

gw_node_t *gw_registry_find_mac(gw_registry_t *reg, const uint8_t mac[MESH_MAC_LEN]);
gw_node_t *gw_registry_find_id(gw_registry_t *reg, uint16_t node_id);
size_t gw_registry_count(const gw_registry_t *reg);
size_t gw_registry_count_state(const gw_registry_t *reg, gw_node_state_t state);

/**
 * Register or refresh a node from a JOIN. Resets its sequence tracker (a JOIN
 * starts a new sequence space) and marks it ONLINE.
 */
gw_join_result_t gw_registry_join(gw_registry_t *reg, const uint8_t mac[MESH_MAC_LEN], uint16_t node_id,
                                  const mesh_join_payload_t *join, uint64_t now_ms, gw_node_t **out);

/**
 * Account for a frame from a known node: liveness refresh + sequence tracking.
 * @param transition set if the node came back from SUSPECT/OFFLINE.
 */
mesh_seq_result_t gw_registry_on_frame(gw_registry_t *reg, gw_node_t *node, uint16_t seq, int8_t rssi,
                                       uint64_t now_ms, uint32_t *gap, gw_transition_t *transition,
                                       bool *transitioned);

/** Silence thresholds for a node, derived from its announced heartbeat interval. */
uint32_t gw_registry_suspect_ms(const gw_registry_t *reg, const gw_node_t *node);
uint32_t gw_registry_offline_ms(const gw_registry_t *reg, const gw_node_t *node);

/** Evaluate silence timers; writes up to @p max transitions and returns how many. */
size_t gw_registry_tick(gw_registry_t *reg, uint64_t now_ms, gw_transition_t *out, size_t max);

/** Remove a node by id. Returns true if it existed (MAC copied to @p mac_out if non-NULL). */
bool gw_registry_forget(gw_registry_t *reg, uint16_t node_id, uint8_t mac_out[MESH_MAC_LEN]);

/* Persistence: versioned, CRC-protected blob of node identities. */
size_t gw_registry_blob_size(const gw_registry_t *reg);
bool gw_registry_serialize(const gw_registry_t *reg, uint8_t *buf, size_t cap, size_t *len);
/** Restores identities (state OFFLINE). Returns number of nodes restored, or -1 if corrupt. */
int gw_registry_deserialize(gw_registry_t *reg, const uint8_t *buf, size_t len);

const char *gw_node_state_str(gw_node_state_t s);

#ifdef __cplusplus
}
#endif
