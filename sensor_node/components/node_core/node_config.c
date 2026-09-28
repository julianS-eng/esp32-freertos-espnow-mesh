/*
 * SPDX-License-Identifier: MIT
 */
#include "node_config.h"

#include <string.h>

void node_config_defaults(node_config_t *cfg, uint16_t node_id, uint32_t report_ms, uint32_t hb_ms)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->schema = NODE_CONFIG_SCHEMA;
    cfg->node_id = node_id;
    cfg->report_interval_ms = report_ms;
    cfg->heartbeat_interval_ms = hb_ms;
}

bool node_config_is_valid(const node_config_t *cfg)
{
    return cfg->schema == NODE_CONFIG_SCHEMA && cfg->node_id != MESH_GATEWAY_NODE_ID &&
           cfg->node_id != MESH_BROADCAST_NODE_ID && cfg->report_interval_ms >= NODE_REPORT_MS_MIN &&
           cfg->report_interval_ms <= NODE_REPORT_MS_MAX && cfg->heartbeat_interval_ms >= NODE_HB_MS_MIN &&
           cfg->heartbeat_interval_ms <= NODE_HB_MS_MAX;
}

static node_cfg_result_t set_u32(uint32_t *field, uint32_t value, uint32_t lo, uint32_t hi)
{
    if (value < lo || value > hi) {
        return NODE_CFG_INVALID;
    }
    if (*field == value) {
        return NODE_CFG_UNCHANGED;
    }
    *field = value;
    return NODE_CFG_APPLIED;
}

node_cfg_result_t node_config_apply(node_config_t *cfg, uint8_t key, uint32_t value)
{
    switch ((mesh_config_key_t)key) {
    case MESH_CFG_REPORT_INTERVAL_MS:
        return set_u32(&cfg->report_interval_ms, value, NODE_REPORT_MS_MIN, NODE_REPORT_MS_MAX);
    case MESH_CFG_HEARTBEAT_INTERVAL_MS:
        return set_u32(&cfg->heartbeat_interval_ms, value, NODE_HB_MS_MIN, NODE_HB_MS_MAX);
    case MESH_CFG_REBOOT:
        return NODE_CFG_REBOOT;
    default:
        return NODE_CFG_INVALID;
    }
}
