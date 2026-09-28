/*
 * SPDX-License-Identifier: MIT
 *
 * node_config.h - Persistent node configuration: schema, defaults, range
 * validation and remote updates (CONFIG frames). Pure C; the NVS glue lives
 * in main/node_storage.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mesh_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bump when the layout of node_config_t changes; older blobs are discarded. */
#define NODE_CONFIG_SCHEMA 1u

#define NODE_REPORT_MS_MIN 100u
#define NODE_REPORT_MS_MAX 3600000u
#define NODE_HB_MS_MIN 1000u
#define NODE_HB_MS_MAX 600000u

typedef struct {
    uint16_t schema;
    uint16_t node_id;
    uint32_t report_interval_ms;
    uint32_t heartbeat_interval_ms;
} node_config_t;

typedef enum {
    NODE_CFG_APPLIED = 0,    /**< Value changed; caller must persist it. */
    NODE_CFG_UNCHANGED = 1,  /**< Same value; nothing to persist. */
    NODE_CFG_REBOOT = 2,     /**< Caller must acknowledge and restart. */
    NODE_CFG_INVALID = -1,   /**< Unknown key or out-of-range value; config untouched. */
} node_cfg_result_t;

void node_config_defaults(node_config_t *cfg, uint16_t node_id, uint32_t report_ms, uint32_t hb_ms);

/** True if every field is within range and the schema matches. */
bool node_config_is_valid(const node_config_t *cfg);

/** Apply a CONFIG key/value received from the gateway. */
node_cfg_result_t node_config_apply(node_config_t *cfg, uint8_t key, uint32_t value);

#ifdef __cplusplus
}
#endif
