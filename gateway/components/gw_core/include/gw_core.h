/*
 * SPDX-License-Identifier: MIT
 *
 * gw_core.h - Platform-independent gateway logic (pure C, host-testable).
 *
 * gw_core_handle_frame() is the complete per-frame decision procedure:
 * validate -> registry/sequence accounting -> reply (ACK / JOIN_ACK) ->
 * output records. The firmware's RX task and the host simulator both call it,
 * so the behaviour measured in simulation is the behaviour of the firmware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_registry.h"
#include "mesh_protocol.h"
#include "mesh_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAX_TASKS 6

/** Kinds of output records (each becomes one JSON line). */
typedef enum {
    GW_OUT_BOOT = 0,
    GW_OUT_GW_STATE,
    GW_OUT_JOIN,
    GW_OUT_DATA,
    GW_OUT_HEARTBEAT,
    GW_OUT_DUP,
    GW_OUT_NODE_STATE,
    GW_OUT_RX_ERROR,
    GW_OUT_LINK,
    GW_OUT_GW_STATS,
    GW_OUT_CONFIG,
    GW_OUT_INFO,
} gw_out_kind_t;

/** Non-codec receive errors (reported with the codec errors in rx_error). */
#define GW_RXERR_UNKNOWN_NODE (-100)
#define GW_RXERR_BAD_DIRECTION (-101)
#define GW_RXERR_BUSY (-102)

typedef struct {
    uint32_t uptime_s;
    uint32_t rx_frames;
    uint32_t rx_errors;
    uint32_t rx_queue_drops;
    uint32_t out_queue_drops;
    uint32_t out_queue_peak;
    uint32_t acks_sent;
    uint32_t busy_acks;
    uint32_t unknown_node;
    uint32_t duplicates;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t free_psram;
    uint8_t nodes_total;
    uint8_t nodes_online;
    uint8_t nodes_suspect;
    uint8_t nodes_offline;
    uint8_t task_count;
    uint16_t stack_hwm[GW_MAX_TASKS];
    const char *task_names[GW_MAX_TASKS]; /* static strings */
} gw_stats_t;

typedef struct {
    gw_out_kind_t kind;
    uint64_t ts_ms;
    uint16_t node_id;
    uint8_t mac[MESH_MAC_LEN];
    /* Frame metadata (JOIN / DATA / HEARTBEAT / DUP) */
    uint16_t seq;
    uint8_t attempt;
    int8_t rssi;
    uint8_t seq_result; /* mesh_seq_result_t */
    uint32_t gap;
    uint32_t node_uptime_ms;
    union {
        mesh_data_payload_t data;
        mesh_heartbeat_payload_t hb;
        struct {
            mesh_join_payload_t p;
            uint8_t status; /* mesh_join_status_t */
            bool is_new;
        } join;
        gw_transition_t node_state;
        struct {
            uint8_t from;
            uint8_t to;
            uint8_t event;
        } gw_state;
        struct {
            int16_t err;
            uint8_t len;
        } rx_error;
        struct {
            mesh_seq_tracker_t seq;
            uint8_t state;
            int8_t rssi;
            uint32_t rx_errors;
        } link;
        gw_stats_t stats;
        struct {
            uint8_t key;
            uint32_t value;
            uint8_t status;   /* mesh_ack_status_t, or 0xFF when no ACK was received */
            uint8_t attempts;
        } config;
        struct {
            uint32_t fw_version;
            uint8_t channel;
            bool encryption;
            uint8_t capacity;
            int8_t restored; /* nodes restored from NVS, -1 if the blob was corrupt */
        } boot;
        char info[96];
    } u;
} gw_out_t;

typedef struct {
    gw_registry_t reg;
    uint16_t tx_seq;
    uint8_t wifi_channel;
    struct {
        uint32_t rx_frames;
        uint32_t rx_errors;
        uint32_t acks_sent;
        uint32_t busy_acks;
        uint32_t unknown_node;
        uint32_t duplicates;
        uint32_t joins;
    } cnt;
} gw_core_t;

#define GW_RX_MAX_OUT 2

typedef struct {
    /* Reply to transmit (unicast to reply_mac). */
    bool reply;
    bool reply_needs_temp_peer; /* rejection to a MAC we will not keep as a peer */
    uint8_t reply_mac[MESH_MAC_LEN];
    uint8_t reply_buf[MESH_FRAME_MAX];
    size_t reply_len;
    /* Side effects for the platform layer. */
    bool add_peer;        /* register reply_mac as (encrypted) ESP-NOW peer before replying */
    bool persist;         /* registry identity changed: save to NVS */
    bool ack_for_gateway; /* a node acknowledged one of our CONFIG frames */
    mesh_ack_payload_t ack;
    uint16_t ack_from_node;
    /* Records to log. */
    size_t n_out;
    gw_out_t out[GW_RX_MAX_OUT];
} gw_rx_result_t;

void gw_core_init(gw_core_t *core, uint8_t capacity, const gw_liveness_policy_t *policy, uint8_t wifi_channel);

/**
 * Process one received ESP-NOW frame.
 * @param can_forward false when the output queue is full: DATA/HEARTBEAT are
 *        answered with ACK(BUSY) and NOT accounted, so the node retries later.
 */
void gw_core_handle_frame(gw_core_t *core, const uint8_t mac[MESH_MAC_LEN], const uint8_t *data, size_t len,
                          int8_t rssi, uint64_t now_ms, bool can_forward, gw_rx_result_t *res);

/** Liveness evaluation; fills NODE_STATE records. */
size_t gw_core_tick(gw_core_t *core, uint64_t now_ms, gw_out_t *out, size_t max);

/** One LINK summary record per registered node. */
size_t gw_core_link_records(gw_core_t *core, uint64_t now_ms, gw_out_t *out, size_t max);

/** Encode a CONFIG frame for @p node_id. Returns false if the node is unknown. */
bool gw_core_build_config(gw_core_t *core, uint16_t node_id, uint8_t key, uint32_t value, uint64_t now_ms,
                          uint8_t *buf, size_t *len, uint16_t *seq, uint8_t mac_out[MESH_MAC_LEN]);

/** Fill the counters of @p s that gw_core owns (registry + frame counters). */
void gw_core_fill_stats(const gw_core_t *core, gw_stats_t *s);

#ifdef __cplusplus
}
#endif
