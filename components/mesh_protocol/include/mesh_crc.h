/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_crc.h - CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection,
 * no final XOR). Check value for "123456789" is 0x29B1.
 *
 * The 802.11 FCS already protects each ESP-NOW frame over the air. The
 * application CRC exists for end-to-end integrity: it catches buffer handling
 * bugs, truncated copies between the Wi-Fi task and our queues, and frames
 * produced by incompatible firmware that happen to share the magic byte.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_CRC16_INIT 0xFFFFu

/** Table-driven CRC update; chain calls by passing the previous result as @p crc. */
uint16_t mesh_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

/** Convenience wrapper: CRC of a single buffer starting from MESH_CRC16_INIT. */
static inline uint16_t mesh_crc16(const uint8_t *data, size_t len)
{
    return mesh_crc16_update(MESH_CRC16_INIT, data, len);
}

/** Bit-by-bit reference implementation (slow). Used by tests to validate the table. */
uint16_t mesh_crc16_reference(uint16_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
