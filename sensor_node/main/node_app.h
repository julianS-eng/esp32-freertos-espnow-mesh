/*
 * SPDX-License-Identifier: MIT
 *
 * node_app.h - Shared state of the sensor-node application.
 *
 * Concurrency map (see docs/ARCHITECTURE.md for the diagram):
 *   tx_q      sensor/heartbeat -> tx       readings and heartbeats (drop-oldest)
 *   ack_q     Wi-Fi task (rx cb) -> tx     application ACKs
 *   ctrl_q    Wi-Fi task (rx cb) -> tx     JOIN_ACK and CONFIG frames
 *   link_eg   Wi-Fi task (send cb) -> tx   MAC-layer send completion; JOINED flag
 *   alive_eg  all tasks -> supervisor      liveness check-ins
 *   state_mtx                              protects cfg + stats (priority inheritance)
 */
#pragma once

#include <stdint.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mesh_protocol.h"
#include "mesh_util.h"
#include "node_config.h"
#include "sensor_types.h"

#define NODE_FW_MAJOR 1
#define NODE_FW_MINOR 0
#define NODE_FW_PATCH 0

typedef enum {
    NODE_TASK_SENSOR = 0,
    NODE_TASK_TX,
    NODE_TASK_HEARTBEAT,
    NODE_TASK_SUPERVISOR,
    NODE_TASK_COUNT
} node_task_id_t;
_Static_assert(NODE_TASK_COUNT <= MESH_HB_MAX_TASKS, "heartbeat carries at most MESH_HB_MAX_TASKS HWMs");

/* link_eg bits */
#define LINK_SEND_OK BIT0
#define LINK_SEND_FAIL BIT1
#define LINK_JOINED BIT2

typedef enum { TX_ITEM_DATA = 0, TX_ITEM_HEARTBEAT = 1 } tx_item_kind_t;

typedef struct {
    tx_item_kind_t kind;
    union {
        sensor_reading_t reading;
        mesh_heartbeat_payload_t hb;
    } u;
} tx_item_t;

typedef struct {
    uint16_t seq;
    uint8_t attempt;
    uint8_t status;
    int8_t rssi;
} ack_evt_t;

typedef struct {
    uint8_t mac[MESH_MAC_LEN];
    int8_t rssi;
    mesh_frame_t frame;
} ctrl_evt_t;

typedef struct {
    uint32_t tx_ok;
    uint32_t tx_retries;
    uint32_t tx_failed;
    uint32_t queue_drops;
    uint64_t rtt_sum_us; /* window since the last heartbeat */
    uint32_t rtt_count;
    uint32_t rtt_max_us;
    uint32_t last_rtt_us;
    int8_t last_ack_rssi;
} node_stats_t;

typedef struct {
    node_config_t cfg;
    uint32_t boot_count;
    uint8_t reset_reason;
    bool radio_ok;
    bool gateway_pinned; /* NODE_GATEWAY_MAC configured */
    uint8_t gw_mac[MESH_MAC_LEN];

    QueueHandle_t tx_q;
    QueueHandle_t ack_q;
    QueueHandle_t ctrl_q;
    EventGroupHandle_t link_eg;
    EventGroupHandle_t alive_eg;
    SemaphoreHandle_t state_mtx;

    TaskHandle_t task[NODE_TASK_COUNT];
    uint32_t task_stack[NODE_TASK_COUNT];
    const char *task_name[NODE_TASK_COUNT];

    node_stats_t stats; /* guarded by state_mtx */

    /* Written only by the Wi-Fi task callbacks (single writer, 32-bit aligned
     * stores are atomic on Xtensa), read by the heartbeat task. */
    volatile uint32_t rx_invalid;
    volatile uint32_t rx_dropped;
} node_ctx_t;

/* node_storage.c */
esp_err_t node_storage_init(void);
esp_err_t node_storage_load(node_config_t *cfg, uint16_t default_id);
esp_err_t node_storage_save(const node_config_t *cfg);
uint32_t node_storage_bump_boot_count(void);

/* node_tasks.c */
/** Create IPC objects, bring up the radio (non-fatal) and start all tasks. */
esp_err_t node_tasks_start(node_ctx_t *n);

/** Thread-safe copy of the current configuration. */
node_config_t node_cfg_snapshot(node_ctx_t *n);

static inline uint32_t node_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
