/*
 * SPDX-License-Identifier: MIT
 *
 * gw_json.h - One JSON object per line ("JSON Lines") for every gateway
 * output record. Deterministic, allocation-free, no floating point: values are
 * fixed-point x1000 printed with integer arithmetic, so host and target
 * produce byte-identical output. Schema: docs/PROTOCOL.md, section "Gateway
 * JSON Lines".
 */
#pragma once

#include <stddef.h>

#include "gw_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_JSON_SCHEMA_VERSION 1
#define GW_JSON_LINE_MAX 768

/**
 * Format @p rec as a single JSON object followed by '\n'.
 * @return number of bytes written (excluding NUL), or -1 if @p cap is too small.
 */
int gw_json_format(const gw_out_t *rec, char *buf, size_t cap);

/** Format a fixed-point x1000 value ("-12.5", "3", "0.001"). Returns chars written or -1. */
int gw_json_milli(int32_t v, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
